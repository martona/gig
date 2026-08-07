#pragma once

#include <optional>
#include <string>
#include <vector>

namespace gig {

class SettingsStore;

// Multi-server connection registry over the SettingsStore.
//
// Layout:
//   connections/                the synced subtree
//     mtime                     epoch seconds of the last add/edit/delete
//                               anywhere in the subtree (future-sync hook)
//     <id>/                     one leaf per saved server; <id> is a
//                               deterministic hash of the connection's URL
//       base, url, user, password (encrypted), insecure,
//       ca, cert, key, login-refresh
//   active-connection           ROOT value (deliberately OUTSIDE the subtree
//                               and its mtime): which server THIS DEVICE is
//                               currently watching is per-device state, not
//                               part of the sync payload.
//
// Identity is URL-only: the leaf name is a hash of the (trimmed) base URL --
// or the bare stream `url` when no base is set -- so the same server can be
// stored only once, and editing the URL reparents the entry (new id, old leaf
// removed by the caller). Everything else (view/display/burn-in settings,
// software decode, poll tuning, window geometry, cert pins -- already
// host-keyed under pins/) stays global at the root.

// One saved Frigate connection. Field semantics match AppConfig's connection
// half: `baseUrl` enables discovery; `url` is the single-stream fallback used
// only when baseUrl is empty; ca/cert/key are the PEM escape hatches (no UI).
struct ConnectionInfo {
    std::string baseUrl;
    std::string url;
    std::string user;
    std::string password;
    bool insecure = false; // skip server-cert verification
    std::string caFile;
    std::string certFile;
    std::string keyFile;
    int loginRefreshSeconds = 600;

    // The URL that names this connection: baseUrl if set, else url, trimmed
    // of whitespace and trailing slashes. Empty = not a storable connection.
    std::string identityUrl() const;

    // The leaf name: FNV-1a 64 of identityUrl() as 16 hex chars.
    std::string id() const;

    // Chooser/list label: "host" or "host:port" (default ports elided),
    // falling back to the raw URL when it doesn't parse.
    std::string displayName() const;

    // displayName plus the user ("host:port  (viewer)") -- the row text every
    // list/chooser surface shows, so they all disambiguate the same way.
    std::string listLabel() const;
};

namespace connections {

// Stored leaf ids, sorted (arbitrary but stable order for lists/choosers).
std::vector<std::string> ids(const SettingsStore& store);

// Load one leaf; nullopt for a missing/ghost entry (no base AND no url).
std::optional<ConnectionInfo> load(const SettingsStore& store, const std::string& id);

// Write (or overwrite) the connection's leaf and bump the subtree mtime.
// Returns the leaf id. The caller owns removing a superseded leaf when an
// edited URL changed the id.
std::string save(SettingsStore& store, const ConnectionInfo& info);

// Delete a leaf (and its secrets) and bump the mtime; clears the active
// pointer if it referenced this id. Missing/empty id is a no-op.
void remove(SettingsStore& store, const std::string& id);

// Commit a STAGED edit of the whole list (a settings dialog's working copy):
// saves new/changed entries, removes leaves absent from `items`, and points
// the active pointer at items[activeIndex] (out-of-range, an empty list, or
// an unstorable entry clears it). Unchanged entries are not rewritten, so
// the subtree mtime moves only on real edits. Entries with an empty identity
// URL are skipped (the dialogs block them). Returns the active id ("" = none).
std::string applyStaged(SettingsStore& store, const std::vector<ConnectionInfo>& items, int activeIndex);

// The active leaf id, "" = none. activeOrFallback additionally repairs a
// missing/dangling pointer to the first stored connection (persisting the
// repair) so a deleted-elsewhere active entry can't wedge startup.
std::string activeId(const SettingsStore& store);
std::string activeOrFallback(SettingsStore& store);
void setActiveId(SettingsStore& store, const std::string& id);

// One-time, idempotent migration of the pre-multi-connection root values
// (base/url/user/password/insecure/ca/cert/key/login-refresh) into a
// connections/ leaf + active pointer; the migrated root values are removed
// and schema-version becomes 2. Returns true if a migration ran. TEMPORARY:
// removable once every install has crossed over (the app is not public).
bool migrateFromRoot(SettingsStore& store);

} // namespace connections
} // namespace gig
