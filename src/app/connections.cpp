#include "app/connections.h"

#include "log.hpp"
#include "net/url.h"
#include "platform/settings_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace gig {
namespace {

std::string trimmedWhitespace(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    return value.substr(start);
}

std::string leafPath(const std::string& id, const char* name)
{
    return "connections/" + id + "/" + name;
}

void bumpMtime(SettingsStore& store)
{
    // The subtree's single "anything changed" stamp for the future sync.
    store.setInt("connections/mtime", static_cast<std::int64_t>(std::time(nullptr)));
}

// The secret-free subset of a leaf: enough for identity + labels without a
// Keychain/DPAPI round-trip per entry.
ConnectionInfo liteInfo(const SettingsStore& store, const std::string& id)
{
    ConnectionInfo info;
    info.baseUrl = store.getString(leafPath(id, "base")).value_or(std::string());
    info.url = store.getString(leafPath(id, "url")).value_or(std::string());
    info.user = store.getString(leafPath(id, "user")).value_or(std::string());
    return info;
}

} // namespace

std::string ConnectionInfo::identityUrl() const
{
    return trimTrailingSlashes(trimmedWhitespace(baseUrl.empty() ? url : baseUrl));
}

std::string ConnectionInfo::id() const
{
    // FNV-1a 64 over the identity URL: deterministic, dependency-free, and
    // registry/defaults-safe as 16 hex chars. Collisions across a handful of
    // user-entered URLs are not a realistic concern.
    const std::string identity = identityUrl();
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : identity) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(hex);
}

std::string ConnectionInfo::displayName() const
{
    try {
        // "host" / "host:port" with default ports elided -- the natural label
        // for a chooser ("frigate.lan:9971").
        return hostHeader(parseUrl(identityUrl()));
    } catch (const std::exception&) {
        return identityUrl(); // malformed URL: show it verbatim
    }
}

std::string ConnectionInfo::listLabel() const
{
    std::string label = displayName();
    if (!user.empty()) {
        label += "  (" + user + ")";
    }
    return label;
}

namespace connections {

std::vector<std::string> ids(const SettingsStore& store)
{
    std::vector<std::string> out = store.listSubkeys("connections");
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> loadableIds(const SettingsStore& store)
{
    std::vector<std::string> out;
    for (const std::string& id : ids(store)) {
        if (!liteInfo(store, id).identityUrl().empty()) {
            out.push_back(id);
        }
    }
    return out;
}

std::vector<std::string> listLabels(const SettingsStore& store)
{
    std::vector<std::string> out;
    for (const std::string& id : ids(store)) {
        const ConnectionInfo info = liteInfo(store, id);
        if (!info.identityUrl().empty()) {
            out.push_back(info.listLabel());
        }
    }
    return out;
}

std::optional<ConnectionInfo> load(const SettingsStore& store, const std::string& id)
{
    if (id.empty()) {
        return std::nullopt;
    }
    ConnectionInfo info;
    info.baseUrl = store.getString(leafPath(id, "base")).value_or(std::string());
    info.url = store.getString(leafPath(id, "url")).value_or(std::string());
    if (info.baseUrl.empty() && info.url.empty()) {
        return std::nullopt; // ghost/partial leaf: not a usable connection
    }
    info.user = store.getString(leafPath(id, "user")).value_or(std::string());
    info.password = store.getString(leafPath(id, "password"), /*encrypted=*/true).value_or(std::string());
    info.insecure = store.getBool(leafPath(id, "insecure")).value_or(false);
    info.caFile = store.getString(leafPath(id, "ca")).value_or(std::string());
    info.certFile = store.getString(leafPath(id, "cert")).value_or(std::string());
    info.keyFile = store.getString(leafPath(id, "key")).value_or(std::string());
    info.loginRefreshSeconds = static_cast<int>(store.getInt(leafPath(id, "login-refresh")).value_or(600));
    return info;
}

std::string save(SettingsStore& store, const ConnectionInfo& info)
{
    const std::string id = info.id();
    store.setString(leafPath(id, "base"), info.baseUrl);
    store.setString(leafPath(id, "url"), info.url);
    store.setString(leafPath(id, "user"), info.user);
    if (info.password.empty()) {
        // Mirror the old root behavior: no empty encrypted blobs.
        store.remove(leafPath(id, "password"));
    } else {
        store.setString(leafPath(id, "password"), info.password, /*encrypt=*/true);
    }
    store.setBool(leafPath(id, "insecure"), info.insecure);
    store.setString(leafPath(id, "ca"), info.caFile);
    store.setString(leafPath(id, "cert"), info.certFile);
    store.setString(leafPath(id, "key"), info.keyFile);
    store.setInt(leafPath(id, "login-refresh"), info.loginRefreshSeconds);
    bumpMtime(store);
    return id;
}

namespace {

// Field-wise equality of the STORED representation (everything save() writes),
// so applyStaged can skip rewriting -- and mtime-dirtying -- unchanged leaves.
bool sameStored(const ConnectionInfo& a, const ConnectionInfo& b)
{
    return a.baseUrl == b.baseUrl && a.url == b.url && a.user == b.user
        && a.password == b.password && a.insecure == b.insecure
        && a.caFile == b.caFile && a.certFile == b.certFile && a.keyFile == b.keyFile
        && a.loginRefreshSeconds == b.loginRefreshSeconds;
}

} // namespace

std::string applyStaged(SettingsStore& store, const std::vector<ConnectionInfo>& items, int activeIndex)
{
    std::vector<std::string> stagedIds;
    stagedIds.reserve(items.size());
    for (const ConnectionInfo& item : items) {
        if (!item.identityUrl().empty()) {
            stagedIds.push_back(item.id());
        }
    }
    // Deletions first: leaves that vanished from the staged list.
    for (const std::string& id : ids(store)) {
        if (std::find(stagedIds.begin(), stagedIds.end(), id) == stagedIds.end()) {
            remove(store, id);
        }
    }
    // Then additions/edits; unchanged entries are left alone.
    for (const ConnectionInfo& item : items) {
        if (item.identityUrl().empty()) {
            continue;
        }
        const std::optional<ConnectionInfo> existing = load(store, item.id());
        if (!existing || !sameStored(*existing, item)) {
            save(store, item);
        }
    }
    std::string active;
    if (activeIndex >= 0 && activeIndex < static_cast<int>(items.size())
        && !items[static_cast<std::size_t>(activeIndex)].identityUrl().empty()) {
        active = items[static_cast<std::size_t>(activeIndex)].id();
    }
    setActiveId(store, active);
    return active;
}

void remove(SettingsStore& store, const std::string& id)
{
    if (id.empty()) {
        return;
    }
    store.removeSubtree("connections/" + id);
    bumpMtime(store);
    if (activeId(store) == id) {
        // Leave repair (falling back to another entry) to activeOrFallback;
        // a dangling pointer must simply never persist.
        store.remove("active-connection");
    }
}

std::string activeId(const SettingsStore& store)
{
    return store.getString("active-connection").value_or(std::string());
}

void setActiveId(SettingsStore& store, const std::string& id)
{
    if (id.empty()) {
        store.remove("active-connection");
    } else {
        store.setString("active-connection", id);
    }
}

std::string activeOrFallback(SettingsStore& store)
{
    const std::string current = activeId(store);
    if (!current.empty() && load(store, current)) {
        return current;
    }
    // Missing or dangling: repair to the first loadable connection (sorted
    // order -- arbitrary but stable), or clear when none remain.
    for (const std::string& id : ids(store)) {
        if (load(store, id)) {
            setActiveId(store, id);
            return id;
        }
    }
    setActiveId(store, std::string());
    return std::string();
}

bool migrateFromRoot(SettingsStore& store)
{
    if (!store.listSubkeys("connections").empty()) {
        return false; // already on the multi-connection schema
    }
    ConnectionInfo info;
    info.baseUrl = store.getString("base").value_or(std::string());
    info.url = store.getString("url").value_or(std::string());
    if (info.identityUrl().empty()) {
        return false; // fresh install: nothing to carry over
    }
    info.user = store.getString("user").value_or(std::string());
    info.password = store.getString("password", /*encrypted=*/true).value_or(std::string());
    info.insecure = store.getBool("insecure").value_or(false);
    info.caFile = store.getString("ca").value_or(std::string());
    info.certFile = store.getString("cert").value_or(std::string());
    info.keyFile = store.getString("key").value_or(std::string());
    info.loginRefreshSeconds = static_cast<int>(store.getInt("login-refresh").value_or(600));

    const std::string id = save(store, info);
    setActiveId(store, id);
    for (const char* key : { "base", "url", "user", "password", "insecure",
                             "ca", "cert", "key", "login-refresh" }) {
        store.remove(key);
    }
    store.setInt("schema-version", 2);
    logInfo() << "settings: migrated root connection to connections/" << id
              << " (" << info.displayName() << ")";
    return true;
}

} // namespace connections
} // namespace gig
