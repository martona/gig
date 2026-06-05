#include "net/tls_session_cache.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <ctime>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace gig {
namespace {

std::time_t sessionExpiresAt(const SSL_SESSION* session)
{
    const std::time_t createdAt = SSL_SESSION_get_time_ex(session);
    const long timeout = SSL_SESSION_get_timeout(session);
    if (createdAt <= 0 || timeout <= 0) {
        return 0;
    }

    const auto maxTime = std::numeric_limits<std::time_t>::max();
    const auto lifetime = static_cast<std::time_t>(timeout);
    if (createdAt > maxTime - lifetime) {
        return maxTime;
    }
    return createdAt + lifetime;
}

bool isSessionValid(const SSL_SESSION* session, std::time_t now)
{
    if (session == nullptr) {
        return false;
    }
    return now < sessionExpiresAt(session);
}

// Process-global ex-data slot used to hand the per-connection cache pointer to
// the SSL_CTX-level new-session callback.
int sessionCacheExDataIndex()
{
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int cacheNewSession(SSL* ssl, SSL_SESSION* session)
{
    const int index = sessionCacheExDataIndex();
    if (index < 0) {
        return 0;
    }

    auto* cache = static_cast<TlsSessionCache*>(SSL_get_ex_data(ssl, index));
    if (cache == nullptr) {
        return 0;
    }

    // Returning 1 tells OpenSSL we retained the session; push() owns it now.
    return cache->push(session) ? 1 : 0;
}

} // namespace

void SslSessionDeleter::operator()(SSL_SESSION* session) const noexcept
{
    SSL_SESSION_free(session);
}

struct TlsSessionCache::Impl {
    explicit Impl(std::size_t maxSize)
        : maxSize(maxSize)
    {
    }

    void pruneExpiredLocked(std::time_t now)
    {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!isSessionValid(it->get(), now)) {
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t maxSize = 16;
    mutable std::mutex mutex;
    std::deque<SslSessionPtr> sessions;
};

TlsSessionCache::TlsSessionCache(std::size_t maxSize)
    : impl_(std::make_unique<Impl>(maxSize))
{
}

TlsSessionCache::~TlsSessionCache() = default;
TlsSessionCache::TlsSessionCache(TlsSessionCache&&) noexcept = default;
TlsSessionCache& TlsSessionCache::operator=(TlsSessionCache&&) noexcept = default;

bool TlsSessionCache::push(SSL_SESSION* session)
{
    const std::time_t now = std::time(nullptr);
    if (impl_->maxSize == 0 || !isSessionValid(session, now)) {
        return false;
    }

    SslSessionPtr ownedSession(session);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pruneExpiredLocked(now);
    impl_->sessions.push_back(std::move(ownedSession));
    while (impl_->sessions.size() > impl_->maxSize) {
        impl_->sessions.pop_front();
    }
    return true;
}

SslSessionPtr TlsSessionCache::pop()
{
    const std::time_t now = std::time(nullptr);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pruneExpiredLocked(now);
    if (impl_->sessions.empty()) {
        return {};
    }

    SslSessionPtr session = std::move(impl_->sessions.front());
    impl_->sessions.pop_front();
    return session;
}

std::size_t TlsSessionCache::size() const
{
    const std::time_t now = std::time(nullptr);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pruneExpiredLocked(now);
    return impl_->sessions.size();
}

void enableSessionCache(SSL_CTX* context)
{
    SSL_CTX_set_session_cache_mode(
        context,
        SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(context, &cacheNewSession);
}

void offerCachedSession(SSL* ssl, TlsSessionCache& cache)
{
    const int index = sessionCacheExDataIndex();
    if (index < 0) {
        return;
    }
    SSL_set_ex_data(ssl, index, &cache);

    SslSessionPtr session = cache.pop();
    if (session == nullptr) {
        return;
    }
    if (SSL_set_session(ssl, session.get()) != 1) {
        ERR_clear_error();
    }
}

bool sessionWasReused(SSL* ssl)
{
    return SSL_session_reused(ssl) == 1;
}

} // namespace gig
