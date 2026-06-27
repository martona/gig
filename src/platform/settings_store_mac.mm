#include "platform/settings_store.hpp"

#include "log.hpp"

#include <mutex>
#include <string>

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace gig {
namespace {

// First-light macOS settings backend: plain values in NSUserDefaults (CFPreferences,
// ~/Library/Preferences/<bundle-id>.plist) and secret values (encrypt=true) in the
// Keychain as generic-password items under service "gig". This is interface-complete
// and secure for the single password we store today.
//
// The handoff's locked design -- ONE symmetric key in the Keychain, with which we
// encrypt/decrypt the secret values ourselves -- is the planned refinement for when
// per-server creds arrive (it keeps the Keychain to a single fixed item that doesn't
// grow per server). Only the encrypted-value path below changes for that; the plain
// path and the SettingsStore interface stay. Flagged so it isn't mistaken for final.

NSString* toNs(std::string_view text)
{
    return [[NSString alloc] initWithBytes:text.data()
                                    length:static_cast<NSUInteger>(text.size())
                                  encoding:NSUTF8StringEncoding];
}

// Keychain service that namespaces gig's secret items. Derived from the app's
// bundle identifier so it tracks MACOSX_BUNDLE_GUI_IDENTIFIER (the one knob that
// also drives the NSUserDefaults domain); falls back to a literal only if run
// outside a resolvable bundle (where bundleIdentifier is nil).
NSString* keychainService()
{
    NSString* identifier = [[NSBundle mainBundle] bundleIdentifier];
    return identifier.length > 0 ? identifier : @"stream.gig.app";
}

class MacSettingsStore final : public SettingsStore {
public:
    std::optional<std::string> getString(std::string_view key, bool encrypted) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (encrypted) {
            return keychainGet(key);
        }
        NSString* value = [defaults() stringForKey:toNs(key)];
        if (value == nil) {
            return std::nullopt;
        }
        return std::string(value.UTF8String);
    }

    void setString(std::string_view key, std::string_view value, bool encrypt) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (encrypt) {
            keychainSet(key, value);
            return;
        }
        [defaults() setObject:toNs(value) forKey:toNs(key)];
    }

    std::optional<std::int64_t> getInt(std::string_view key) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id object = [defaults() objectForKey:toNs(key)];
        if (![object isKindOfClass:[NSNumber class]]) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>([(NSNumber*)object longLongValue]);
    }

    void setInt(std::string_view key, std::int64_t value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        [defaults() setObject:@(value) forKey:toNs(key)];
    }

    std::optional<bool> getBool(std::string_view key) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id object = [defaults() objectForKey:toNs(key)];
        if (![object isKindOfClass:[NSNumber class]]) {
            return std::nullopt;
        }
        return static_cast<bool>([(NSNumber*)object boolValue]);
    }

    void setBool(std::string_view key, bool value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        [defaults() setObject:@(value) forKey:toNs(key)];
    }

    void remove(std::string_view key) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A value lives in either defaults or the Keychain depending on how it was
        // written; clear both so remove() is correct regardless. Missing is a no-op.
        [defaults() removeObjectForKey:toNs(key)];
        keychainDelete(key);
    }

    std::vector<std::string> listKeys(std::string_view subkey) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string prefix(subkey);
        prefix += '/';
        std::vector<std::string> out;
        NSDictionary<NSString*, id>* all = [defaults() dictionaryRepresentation];
        for (NSString* k in all) {
            const std::string key(k.UTF8String);
            if (key.rfind(prefix, 0) == 0) {
                out.push_back(key.substr(prefix.size()));
            }
        }
        return out;
    }

private:
    static NSUserDefaults* defaults() { return [NSUserDefaults standardUserDefaults]; }

    static NSMutableDictionary* keychainQuery(std::string_view key)
    {
        NSMutableDictionary* query = [NSMutableDictionary dictionary];
        query[(__bridge id)kSecClass] = (__bridge id)kSecClassGenericPassword;
        query[(__bridge id)kSecAttrService] = keychainService();
        query[(__bridge id)kSecAttrAccount] = toNs(key);
        return query;
    }

    static std::optional<std::string> keychainGet(std::string_view key)
    {
        NSMutableDictionary* query = keychainQuery(key);
        query[(__bridge id)kSecReturnData] = @YES;
        query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;

        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
        if (status != errSecSuccess || result == nullptr) {
            return std::nullopt;
        }
        NSData* data = (__bridge_transfer NSData*)result;
        return std::string(static_cast<const char*>(data.bytes), data.length);
    }

    static void keychainSet(std::string_view key, std::string_view value)
    {
        // Delete any existing item, then add a fresh one, so gig (the writer) OWNS the
        // keychain item. A signed gig then reads its own item without the access
        // prompt -- whereas updating an item created by the `security` CLI keeps the
        // CLI as owner and prompts once. (Pairs with the native settings UI writing
        // the password.)
        SecItemDelete((__bridge CFDictionaryRef)keychainQuery(key));
        NSMutableDictionary* add = keychainQuery(key);
        add[(__bridge id)kSecValueData] = [NSData dataWithBytes:value.data() length:value.size()];
        add[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
        const OSStatus status = SecItemAdd((__bridge CFDictionaryRef)add, nullptr);
        if (status != errSecSuccess) {
            logWarning() << "keychain write failed for '" << std::string(key)
                         << "' (OSStatus " << static_cast<long>(status) << ")";
        }
    }

    static void keychainDelete(std::string_view key)
    {
        SecItemDelete((__bridge CFDictionaryRef)keychainQuery(key));
    }

    mutable std::mutex mutex_;
};

} // namespace

std::shared_ptr<SettingsStore> openSettingsStore()
{
    return std::make_shared<MacSettingsStore>();
}

} // namespace gig
