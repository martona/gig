#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gig {

// Platform-neutral read/write settings facade -- the single source of config,
// replacing the old read-only gig.ini. Windows backs it with the registry under
// HKCU\Software\gig; values written with `encrypt` are DPAPI-wrapped (CurrentUser
// scope) and stored as binary. The macOS backend (later) maps to a plist +
// Keychain. No platform types (HKEY, etc.) leak through this interface.
//
// Keys are flat names ("base", "poll-interval"); a '/' nests into a subkey
// ("pins/frigate.lan" -> value "frigate.lan" under subkey "pins"), which is how
// the cert-pin store enumerates hosts via listKeys("pins").
//
// Implementations are thread-safe: config is read on the main thread at startup
// and rewritten from the settings dialog; pins are read on network threads and
// written from the UI thread.
class SettingsStore {
public:
    virtual ~SettingsStore() = default;

    // `encrypted` must match how the value was written (symmetric). Returns
    // nullopt for a missing value, a type mismatch, or an undecryptable blob.
    virtual std::optional<std::string> getString(std::string_view key, bool encrypted = false) const = 0;
    virtual void setString(std::string_view key, std::string_view value, bool encrypt = false) = 0;

    virtual std::optional<std::int64_t> getInt(std::string_view key) const = 0;
    virtual void setInt(std::string_view key, std::int64_t value) = 0;

    virtual std::optional<bool> getBool(std::string_view key) const = 0;
    virtual void setBool(std::string_view key, bool value) = 0;

    // Remove a single value. Missing is not an error.
    virtual void remove(std::string_view key) = 0;

    // Wipe EVERYTHING this store holds -- config, cert pins, window geometry,
    // secrets. Backs the (temporary) "Forget Settings" onboarding-reset
    // affordance; the store remains usable afterwards (empty).
    virtual void clear() = 0;

    // Value names directly under `subkey` (e.g. "pins"), oldest-first is not
    // guaranteed. Empty if the subkey doesn't exist.
    virtual std::vector<std::string> listKeys(std::string_view subkey) const = 0;
};

// Open the process-wide settings store for this platform (registry on Windows).
// Throws std::runtime_error if the backing store can't be opened.
std::shared_ptr<SettingsStore> openSettingsStore();

} // namespace gig
