#include "platform/settings_store.hpp"

#include <cstring>
#include <iterator>
#include <mutex>
#include <stdexcept>

#include <windows.h>
#include <dpapi.h> // CryptProtectData / CryptUnprotectData (crypt32)

namespace gig {
namespace {

constexpr const wchar_t* kRootSubkey = L"Software\\gig";

std::wstring utf8ToWide(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), needed);
    return wide;
}

std::string wideToUtf8(const wchar_t* value, std::size_t length)
{
    if (length == 0) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, value, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

// Split "pins/frigate.lan" -> subpath L"pins", value L"frigate.lan". A key with
// no '/' yields an empty subpath (value lives directly under the root). Registry
// uses '\' separators, so '/' is translated.
void splitKey(std::string_view key, std::wstring& subpath, std::wstring& value)
{
    const std::size_t slash = key.rfind('/');
    if (slash == std::string_view::npos) {
        subpath.clear();
        value = utf8ToWide(key);
        return;
    }
    subpath = utf8ToWide(key.substr(0, slash));
    for (wchar_t& ch : subpath) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    value = utf8ToWide(key.substr(slash + 1));
}

// HKCU\Software\gig registry store. DPAPI-wrapped values are REG_BINARY; plain
// strings REG_SZ (UTF-16), ints REG_QWORD, bools REG_DWORD.
class WindowsSettingsStore : public SettingsStore {
public:
    WindowsSettingsStore()
    {
        const LSTATUS status = RegCreateKeyExW(
            HKEY_CURRENT_USER, kRootSubkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE, nullptr, &root_, nullptr);
        if (status != ERROR_SUCCESS) {
            throw std::runtime_error("failed to open HKCU\\Software\\gig (error " + std::to_string(status) + ")");
        }
    }

    ~WindowsSettingsStore() override
    {
        if (root_) {
            RegCloseKey(root_);
        }
    }

    std::optional<std::string> getString(std::string_view key, bool encrypted) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (encrypted) {
            std::vector<BYTE> blob;
            if (!readRaw(key, RRF_RT_REG_BINARY, blob) || blob.empty()) {
                return std::nullopt;
            }
            DATA_BLOB in { static_cast<DWORD>(blob.size()), blob.data() };
            DATA_BLOB out {};
            if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
                return std::nullopt; // wrong user, tampered, or not a DPAPI blob
            }
            std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
            LocalFree(out.pbData);
            return plain;
        }

        std::vector<BYTE> raw;
        if (!readRaw(key, RRF_RT_REG_SZ, raw) || raw.size() < sizeof(wchar_t)) {
            return raw.empty() ? std::nullopt : std::optional<std::string>(std::string());
        }
        // REG_SZ comes back NUL-terminated; drop the terminator before converting.
        const auto* chars = reinterpret_cast<const wchar_t*>(raw.data());
        std::size_t count = raw.size() / sizeof(wchar_t);
        while (count > 0 && chars[count - 1] == L'\0') {
            --count;
        }
        return wideToUtf8(chars, count);
    }

    void setString(std::string_view key, std::string_view value, bool encrypt) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (encrypt) {
            DATA_BLOB in { static_cast<DWORD>(value.size()),
                           reinterpret_cast<BYTE*>(const_cast<char*>(value.data())) };
            DATA_BLOB out {};
            if (!CryptProtectData(&in, L"gig", nullptr, nullptr, nullptr, 0, &out)) {
                throw std::runtime_error("CryptProtectData failed for '" + std::string(key) + "'");
            }
            writeRaw(key, REG_BINARY, out.pbData, out.cbData);
            LocalFree(out.pbData);
            return;
        }
        const std::wstring wide = utf8ToWide(value);
        writeRaw(key, REG_SZ, wide.c_str(), static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
    }

    std::optional<std::int64_t> getInt(std::string_view key) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BYTE> raw;
        if (!readRaw(key, RRF_RT_REG_QWORD, raw) || raw.size() != sizeof(std::int64_t)) {
            return std::nullopt;
        }
        std::int64_t value = 0;
        std::memcpy(&value, raw.data(), sizeof(value));
        return value;
    }

    void setInt(std::string_view key, std::int64_t value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writeRaw(key, REG_QWORD, &value, sizeof(value));
    }

    std::optional<bool> getBool(std::string_view key) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BYTE> raw;
        if (!readRaw(key, RRF_RT_REG_DWORD, raw) || raw.size() != sizeof(DWORD)) {
            return std::nullopt;
        }
        DWORD value = 0;
        std::memcpy(&value, raw.data(), sizeof(value));
        return value != 0;
    }

    void setBool(std::string_view key, bool value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const DWORD raw = value ? 1u : 0u;
        writeRaw(key, REG_DWORD, &raw, sizeof(raw));
    }

    void remove(std::string_view key) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::wstring subpath;
        std::wstring value;
        splitKey(key, subpath, value);
        RegDeleteKeyValueW(root_, subpath.empty() ? nullptr : subpath.c_str(), value.c_str());
    }

    void clear() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Drop the whole HKCU\Software\gig tree (values, pins subkey, geometry)
        // and re-create it empty so the open handle stays valid for later writes.
        if (root_) {
            RegCloseKey(root_);
            root_ = nullptr;
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, kRootSubkey);
        const LSTATUS status = RegCreateKeyExW(
            HKEY_CURRENT_USER, kRootSubkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE, nullptr, &root_, nullptr);
        if (status != ERROR_SUCCESS) {
            throw std::runtime_error("failed to re-create HKCU\\Software\\gig (error " + std::to_string(status) + ")");
        }
    }

    std::vector<std::string> listKeys(std::string_view subkey) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        std::wstring sub = utf8ToWide(subkey);
        for (wchar_t& ch : sub) {
            if (ch == L'/') {
                ch = L'\\';
            }
        }

        HKEY hSub = nullptr;
        if (RegOpenKeyExW(root_, sub.c_str(), 0, KEY_READ, &hSub) != ERROR_SUCCESS) {
            return names;
        }
        for (DWORD index = 0;; ++index) {
            wchar_t name[256];
            DWORD nameLen = static_cast<DWORD>(std::size(name));
            const LSTATUS status = RegEnumValueW(hSub, index, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
            if (status != ERROR_SUCCESS) {
                break;
            }
            names.push_back(wideToUtf8(name, nameLen));
        }
        RegCloseKey(hSub);
        return names;
    }

private:
    // Two-pass read of any value at key into `out`. `flags` restricts the
    // accepted registry type. Returns false if missing / wrong type.
    bool readRaw(std::string_view key, DWORD flags, std::vector<BYTE>& out) const
    {
        std::wstring subpath;
        std::wstring value;
        splitKey(key, subpath, value);
        const wchar_t* sub = subpath.empty() ? nullptr : subpath.c_str();

        DWORD bytes = 0;
        LSTATUS status = RegGetValueW(root_, sub, value.c_str(), flags, nullptr, nullptr, &bytes);
        if (status != ERROR_SUCCESS || bytes == 0) {
            return false;
        }
        out.resize(bytes);
        status = RegGetValueW(root_, sub, value.c_str(), flags, nullptr, out.data(), &bytes);
        if (status != ERROR_SUCCESS) {
            return false;
        }
        out.resize(bytes);
        return true;
    }

    void writeRaw(std::string_view key, DWORD type, const void* data, DWORD bytes)
    {
        std::wstring subpath;
        std::wstring value;
        splitKey(key, subpath, value);
        if (subpath.empty()) {
            RegSetValueExW(root_, value.c_str(), 0, type, static_cast<const BYTE*>(data), bytes);
            return;
        }
        // Nested key (e.g. pins\<host>): ensure the subkey exists, then set.
        HKEY hSub = nullptr;
        if (RegCreateKeyExW(root_, subpath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_SET_VALUE, nullptr, &hSub, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hSub, value.c_str(), 0, type, static_cast<const BYTE*>(data), bytes);
            RegCloseKey(hSub);
        }
    }

    HKEY root_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace

std::shared_ptr<SettingsStore> openSettingsStore()
{
    return std::make_shared<WindowsSettingsStore>();
}

} // namespace gig
