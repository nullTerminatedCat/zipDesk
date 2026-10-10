#include "ShellIntegration.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>
#include <shlobj.h>

#include <cstring>
#include <string>
#include <vector>

namespace zipdesk {
namespace {

void setString(const std::wstring& subkey, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
}

void setDefault(const std::wstring& subkey, const std::wstring& value) {
    setString(subkey, nullptr, value);
}

void deleteTree(const std::wstring& subkey) {
    RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
}

void deleteValue(const std::wstring& subkey, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    RegDeleteValueW(key, name);
    RegCloseKey(key);
}

std::wstring nativePath(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    return path;
}

std::wstring normalizedPath(std::wstring path) {
    path = nativePath(std::move(path));
    for (wchar_t& ch : path) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return path;
}

std::wstring quotedCommand(const std::wstring& exePath, const std::wstring& arguments) {
    return L"\"" + exePath + L"\" " + arguments;
}

void deleteProtectedKey(const std::wstring& keyPath) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, WRITE_DAC | READ_CONTROL, &key) == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = KEY_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        wchar_t user[256] = {};
        DWORD userSize = 256;
        GetUserNameW(user, &userSize);
        access.Trustee.ptstrName = user;
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &access, nullptr, &acl) == ERROR_SUCCESS) {
            SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, acl, nullptr);
            LocalFree(acl);
        }
        RegCloseKey(key);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, keyPath.c_str());
}

int32_t narrow32(int64_t value) {
    return static_cast<int32_t>(static_cast<uint32_t>(value & 0xFFFFFFFFLL));
}

int64_t shiftRight(int64_t value, int count) {
    const uint64_t bits = static_cast<uint64_t>(value);
    if ((bits & 0x80000000ULL) != 0) {
        return static_cast<int64_t>((bits >> count) ^ 0xFFFF0000ULL);
    }
    return static_cast<int64_t>(bits >> count);
}

int32_t readInt32(const std::vector<uint8_t>& bytes, int index) {
    int32_t value = 0;
    std::memcpy(&value, bytes.data() + index, sizeof(value));
    return value;
}

std::vector<uint8_t> md5Bytes(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(16);
    HCRYPTPROV provider = 0;
    HCRYPTHASH cryptHash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return {};
    }
    if (CryptCreateHash(provider, CALG_MD5, 0, 0, &cryptHash)) {
        CryptHashData(cryptHash, data.data(), static_cast<DWORD>(data.size()), 0);
        DWORD size = 16;
        CryptGetHashParam(cryptHash, HP_HASHVAL, hash.data(), &size, 0);
        CryptDestroyHash(cryptHash);
    }
    CryptReleaseContext(provider, 0);
    return hash;
}

std::string userChoiceHash(const std::string& baseInfo) {
    std::vector<uint8_t> bytes(baseInfo.size() * 2 + 2, 0);
    for (size_t i = 0; i < baseInfo.size(); ++i) {
        bytes[i * 2] = static_cast<uint8_t>(baseInfo[i]);
    }
    const std::vector<uint8_t> digest = md5Bytes(bytes);
    if (digest.size() != 16) {
        return {};
    }
    const int lengthBase = static_cast<int>(baseInfo.size() * 2 + 2);
    const int length = ((lengthBase & 4) == 0 ? 1 : 0) + static_cast<int>(shiftRight(lengthBase, 2)) - 1;
    if (length <= 1) {
        return {};
    }

    auto mix = [&](int64_t md51, int64_t md52, bool firstPass) {
        int64_t cache = 0;
        int64_t outHash1 = 0;
        int pdata = 0;
        const int counterStart = static_cast<int>(shiftRight(length - 2, 1)) + 1;
        for (int counter = counterStart; counter > 0; --counter) {
            const int32_t r0 = narrow32(readInt32(bytes, pdata) + outHash1);
            const int32_t r1 = firstPass ? readInt32(bytes, pdata + 4) : 0;
            pdata += 8;
            if (firstPass) {
                const int32_t r2a = narrow32(static_cast<int64_t>(r0) * md51 - 0x10FA9605LL * shiftRight(r0, 16));
                const int32_t r2b = narrow32(0x79F8A395LL * r2a + 0x689B6B9FLL * shiftRight(r2a, 16));
                const int32_t r3 = narrow32(0xEA970001LL * r2b - 0x3C101569LL * shiftRight(r2b, 16));
                const int32_t r4 = narrow32(static_cast<int64_t>(r3) + r1);
                const int32_t r5 = narrow32(cache + r3);
                const int32_t r6a = narrow32(static_cast<int64_t>(r4) * md52 - 0x3CE8EC25LL * shiftRight(r4, 16));
                const int32_t r6b = narrow32(0x59C3AF2DLL * r6a - 0x2232E0F1LL * shiftRight(r6a, 16));
                outHash1 = narrow32(0x1EC90001LL * r6b + 0x35BD1EC9LL * shiftRight(r6b, 16));
                cache = narrow32(static_cast<int64_t>(r5) + outHash1);
            } else {
                const int32_t p1 = narrow32(static_cast<int64_t>(r0) * md51);
                const int32_t p2 = narrow32(0xB1110000LL * p1 - 0x30674EEFLL * shiftRight(p1, 16));
                const int32_t p3 = narrow32(0x5B9F0000LL * p2 - 0x78F7A461LL * shiftRight(p2, 16));
                const int32_t p4 = narrow32(0x12CEB96DLL * shiftRight(p3, 16) - 0x46930000LL * p3);
                const int32_t r3 = narrow32(0x1D830000LL * p4 + 0x257E1D83LL * shiftRight(p4, 16));
                const int32_t q1 = narrow32(md52 * (static_cast<int64_t>(r3) + readInt32(bytes, pdata - 4)));
                const int32_t q2 = narrow32(0x16F50000LL * q1 - 0x5D8BE90BLL * shiftRight(q1, 16));
                const int32_t q3 = narrow32(0x96FF0000LL * q2 - 0x2C7C6901LL * shiftRight(q2, 16));
                const int32_t q4 = narrow32(0x2B890000LL * q3 + 0x7C932B89LL * shiftRight(q3, 16));
                outHash1 = narrow32(0x9F690000LL * q4 - 0x405B6097LL * shiftRight(q4, 16));
                cache = narrow32(outHash1 + cache + r3);
            }
        }
        return std::pair<int32_t, int32_t>{static_cast<int32_t>(outHash1), static_cast<int32_t>(cache)};
    };

    const int32_t md5a = readInt32(digest, 0);
    const int32_t md5b = readInt32(digest, 4);
    const auto first = mix((static_cast<int64_t>(md5a) | 1) + 0x69FB0000LL, (static_cast<int64_t>(md5b) | 1) + 0x13DB0000LL, true);
    const auto second = mix(static_cast<int64_t>(md5a) | 1, static_cast<int64_t>(md5b) | 1, false);
    int32_t out[4] = {first.first, first.second, second.first, second.second};
    const int32_t hash1 = out[2] ^ out[0];
    const int32_t hash2 = out[3] ^ out[1];
    uint8_t raw[8] = {};
    std::memcpy(raw, &hash1, 4);
    std::memcpy(raw + 4, &hash2, 4);
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (int i = 0; i < 8; i += 3) {
        const int remain = 8 - i;
        const uint32_t chunk = (static_cast<uint32_t>(raw[i]) << 16) |
                               (remain > 1 ? static_cast<uint32_t>(raw[i + 1]) << 8 : 0) |
                               (remain > 2 ? static_cast<uint32_t>(raw[i + 2]) : 0);
        encoded.push_back(table[(chunk >> 18) & 63]);
        encoded.push_back(table[(chunk >> 12) & 63]);
        encoded.push_back(remain > 1 ? table[(chunk >> 6) & 63] : '=');
        encoded.push_back(remain > 2 ? table[chunk & 63] : '=');
    }
    return encoded;
}

std::string userSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> buffer(size);
    std::string sid;
    if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
                for (const wchar_t* cursor = text; *cursor != 0; ++cursor) {
                    sid.push_back(static_cast<char>(*cursor));
                }
                LocalFree(text);
            }
    }
    CloseHandle(token);
    for (char& ch : sid) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    return sid;
}

std::string experienceString() {
    const std::string fallback = "user choice set via windows user experience {d18b6dd5-6124-4341-9318-804003bafa0b}";
    const wchar_t* path = L"C:\\Windows\\SysWOW64\\shell32.dll";
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return fallback;
    }
    std::vector<uint8_t> data(5 * 1024 * 1024);
    DWORD read = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    const std::wstring text(reinterpret_cast<wchar_t*>(data.data()), read / 2);
    const std::wstring marker = L"User Choice set via Windows User Experience";
    const auto start = text.find(marker);
    if (start == std::wstring::npos) {
        return fallback;
    }
    const auto end = text.find(L'}', start);
    if (end == std::wstring::npos) {
        return fallback;
    }
    std::string result;
    for (size_t i = start; i <= end; ++i) {
        result.push_back(static_cast<char>(towlower(text[i])));
    }
    return result;
}

std::string minuteFileTimeHex() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    local.wSecond = 0;
    local.wMilliseconds = 0;
    SYSTEMTIME utc{};
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    FILETIME fileTime{};
    SystemTimeToFileTime(&utc, &fileTime);
    wchar_t hex[17] = {};
    swprintf_s(hex, L"%08x%08x", fileTime.dwHighDateTime, fileTime.dwLowDateTime);
    std::string result;
    for (const wchar_t ch : hex) {
        if (ch == 0) {
            break;
        }
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

void setUserChoice(const wchar_t* extension, const wchar_t* progId) {
    const std::string ext = [extension]() {
        std::string text;
        for (const wchar_t* cursor = extension; *cursor; ++cursor) {
            text.push_back(static_cast<char>(towlower(*cursor)));
        }
        return text;
    }();
    const std::string id = [progId]() {
        std::string text;
        for (const wchar_t* cursor = progId; *cursor; ++cursor) {
            text.push_back(static_cast<char>(towlower(*cursor)));
        }
        return text;
    }();
    const std::string base = ext + userSid() + id + minuteFileTimeHex() + experienceString();
    const std::string hash = userChoiceHash(base);
    if (hash.empty()) {
        return;
    }
    const std::wstring key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") +
                             extension + L"\\UserChoice";
    deleteProtectedKey(key);
    HKEY opened = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &opened, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    const std::wstring hashWide(hash.begin(), hash.end());
    const std::wstring prog(progId);
    RegSetValueExW(opened, L"Hash", 0, REG_SZ, reinterpret_cast<const BYTE*>(hashWide.c_str()),
                   static_cast<DWORD>((hashWide.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(opened, L"ProgId", 0, REG_SZ, reinterpret_cast<const BYTE*>(prog.c_str()),
                   static_cast<DWORD>((prog.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(opened);
    const std::wstring toast = L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts";
    HKEY toastKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, toast.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &toastKey, nullptr) ==
        ERROR_SUCCESS) {
        const std::wstring name = prog + L"_" + extension;
        const DWORD zero = 0;
        RegSetValueExW(toastKey, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
        RegCloseKey(toastKey);
    }
}

void addCommand(const std::wstring& key, const std::wstring& label, const std::wstring& command) {
    setDefault(key, label);
    setString(key, L"MUIVerb", label);
    setDefault(key + L"\\command", command);
}

void removeOwnedExtension(const wchar_t* extension, const wchar_t* progId) {
    const std::wstring path = std::wstring(L"Software\\Classes\\") + extension;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    wchar_t buffer[64] = {};
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    const bool owned = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes) ==
                           ERROR_SUCCESS &&
                       type == REG_SZ && std::wstring(buffer) == progId;
    RegCloseKey(key);
    if (owned) {
        deleteTree(path);
    }
}

}  // namespace

bool shellIntegrationInstalled(const std::wstring& exePath) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\ZipDesk.zip\\shell\\open\\command", 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buffer[1024] = {};
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    const LONG status =
        RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }
    return normalizedPath(buffer).find(normalizedPath(exePath)) != std::wstring::npos;
}

void writeCommandMenu(const std::wstring& menuClass,
                      const std::vector<std::pair<std::wstring, std::pair<std::wstring, std::wstring>>>& items) {
    const std::wstring root = L"Software\\Classes\\" + menuClass;
    deleteTree(root);
    for (const auto& item : items) {
        const std::wstring key = root + L"\\shell\\" + item.first;
        setDefault(key, item.second.first);
        setString(key, L"MUIVerb", item.second.first);
        setDefault(key + L"\\command", item.second.second);
    }
}

void installCascade(const std::wstring& menuKey, const std::wstring& menuClass, const std::wstring& icon) {
    deleteTree(menuKey);
    setDefault(menuKey, L"日常解压");
    setString(menuKey, L"MUIVerb", L"日常解压");
    setString(menuKey, L"Icon", icon);
    setString(menuKey, L"ExtendedSubCommandsKey", menuClass);
}

void installShellIntegration(const std::wstring& exePathRaw) {
    const std::wstring exePath = nativePath(exePathRaw);
    const std::wstring icon = exePath + L",0";
    const std::wstring open = quotedCommand(exePath, L"\"%1\"");
    const std::wstring extractHere = quotedCommand(exePath, L"--extract-here \"%1\"");
    const std::wstring extractFolder = quotedCommand(exePath, L"--extract-to-folder \"%1\"");
    const std::wstring compressZip = quotedCommand(exePath, L"--compress-zip \"%1\"");
    const std::wstring compress7z = quotedCommand(exePath, L"--compress-7z \"%1\"");

    const std::vector<std::pair<const wchar_t*, const wchar_t*>> archives = {
        {L".zip", L"ZipDesk.zip"},
        {L".7z", L"ZipDesk.7z"},
        {L".rar", L"ZipDesk.rar"},
    };
    const wchar_t* descriptions[] = {L"ZIP 压缩文件", L"7z 压缩文件", L"RAR 压缩文件"};
    int index = 0;
    for (const auto& [extension, progId] : archives) {
        const std::wstring classRoot = std::wstring(L"Software\\Classes\\") + progId;
        setDefault(std::wstring(L"Software\\Classes\\") + extension, progId);
        setDefault(classRoot, descriptions[index]);
        setDefault(classRoot + L"\\DefaultIcon", icon);
        setDefault(classRoot + L"\\shell\\open", L"用日常解压打开");
        setString(classRoot + L"\\shell\\open", L"Icon", icon);
        setDefault(classRoot + L"\\shell\\open\\command", open);

        const std::wstring menu =
            std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + extension + L"\\shell\\ZipDesk";
        installCascade(menu, L"ZipDesk.ExtractMenu", icon);
        deleteTree(std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + extension +
                   L"\\shell\\ZipDesk.ExtractHere");
        deleteTree(std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + extension +
                   L"\\shell\\ZipDesk.ExtractToFolder");
        setUserChoice(extension, progId);
        ++index;
    }

    writeCommandMenu(L"ZipDesk.ExtractMenu",
                     {{L"a_here", {L"解压到当前目录", extractHere}},
                      {L"b_folder", {L"解压到同名文件夹", extractFolder}}});
    writeCommandMenu(L"ZipDesk.CompressMenu",
                     {{L"a_zip", {L"压缩为 zip", compressZip}}, {L"b_7z", {L"压缩为 7z", compress7z}}});
    installCascade(L"Software\\Classes\\*\\shell\\ZipDesk", L"ZipDesk.CompressMenu", icon);
    installCascade(L"Software\\Classes\\Directory\\shell\\ZipDesk", L"ZipDesk.CompressMenu", icon);
    deleteTree(L"Software\\Classes\\*\\shell\\ZipDesk.CompressZip");
    deleteTree(L"Software\\Classes\\*\\shell\\ZipDesk.Compress7z");
    deleteTree(L"Software\\Classes\\Directory\\shell\\ZipDesk.CompressZip");
    deleteTree(L"Software\\Classes\\Directory\\shell\\ZipDesk.Compress7z");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void removeShellIntegration() {
    removeOwnedExtension(L".zip", L"ZipDesk.zip");
    removeOwnedExtension(L".7z", L"ZipDesk.7z");
    removeOwnedExtension(L".rar", L"ZipDesk.rar");
    deleteTree(L"Software\\Classes\\ZipDesk.ExtractMenu");
    deleteTree(L"Software\\Classes\\ZipDesk.CompressMenu");
    deleteTree(L"Software\\Classes\\ZipDesk.zip");
    deleteTree(L"Software\\Classes\\ZipDesk.7z");
    deleteTree(L"Software\\Classes\\ZipDesk.rar");
    deleteTree(L"Software\\Classes\\SystemFileAssociations\\.rar\\shell\\ZipDesk");
    deleteTree(L"Software\\Classes\\SystemFileAssociations\\.zip\\shell\\ZipDesk");
    deleteTree(L"Software\\Classes\\SystemFileAssociations\\.7z\\shell\\ZipDesk");
    deleteTree(L"Software\\Classes\\SystemFileAssociations\\.zip\\shell\\ZipDesk.ExtractHere");
    deleteTree(L"Software\\Classes\\SystemFileAssociations\\.zip\\shell\\ZipDesk.ExtractToFolder");
    deleteTree(L"Software\\Classes\\SystemFileAssociations\\.7z\\shell\\ZipDesk.ExtractHere");
    deleteTree(L"Software\\Classes\\SystemFileAssociations\\.7z\\shell\\ZipDesk.ExtractToFolder");
    deleteTree(L"Software\\Classes\\*\\shell\\ZipDesk");
    deleteTree(L"Software\\Classes\\*\\shell\\ZipDesk.CompressZip");
    deleteTree(L"Software\\Classes\\*\\shell\\ZipDesk.Compress7z");
    deleteTree(L"Software\\Classes\\Directory\\shell\\ZipDesk");
    deleteTree(L"Software\\Classes\\Directory\\shell\\ZipDesk.CompressZip");
    deleteTree(L"Software\\Classes\\Directory\\shell\\ZipDesk.Compress7z");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

}  // namespace zipdesk
