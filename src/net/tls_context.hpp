#pragma once

#include "net/tls_options.h"

#include <boost/asio/ssl/context.hpp>

namespace gig {

// Configure a client TLS context for (optional) server verification and mTLS,
// per TlsOptions. PEM files today; Phase 1 will add the Windows store / CNG
// client cert here so every TLS consumer (control plane + video) flips at once.
//
// Throws std::runtime_error on an unreadable CA/cert/key file or a cert/key
// mismatch. Only included by Boost-aware translation units.
void configureSslContext(boost::asio::ssl::context& context, const TlsOptions& tls);

} // namespace gig
