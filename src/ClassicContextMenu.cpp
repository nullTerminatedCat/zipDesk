#include "ClassicContextMenu.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

namespace zipdesk {
namespace {

constexpr wchar_t kClsidKey[] =
    L"Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}";
constexpr wchar_t kInprocKey[] =
    L"Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32";

using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

}  // namespace

bool isWindows11ForClassicMenu() {
    OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    auto* rtl = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (rtl == nullptr) {
        return false;
    }
    if (rtl(reinterpret_cast<PRTL_OSVERSIONINFOW>(&info)) != 0) {
        return false;
    }
    return info.dwMajorVersion > 10 ||
           (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000);
}

bool isClassicContextMenuEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kInprocKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buffer[8] = {};
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    const LONG status =
        RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes);
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND) {
        // Key exists with unset default — still counts as the classic-menu override.
        return true;
    }
    if (status == ERROR_SUCCESS && type == REG_SZ) {
        return buffer[0] == L'\0';
    }
    return false;
}

bool enableClassicContextMenu() {
    HKEY key = nullptr;
    const LONG created =
        RegCreateKeyExW(HKEY_CURRENT_USER, kInprocKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        return false;
    }
    const wchar_t empty[] = L"";
    const LONG wrote = RegSetValueExW(
        key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
    RegCloseKey(key);
    return wrote == ERROR_SUCCESS && isClassicContextMenuEnabled();
}

bool disableClassicContextMenu() {
    const LONG status = RegDeleteTreeW(HKEY_CURRENT_USER, kClsidKey);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    return !isClassicContextMenuEnabled();
}

bool restartWindowsExplorer() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (tray != nullptr) {
        GetWindowThreadProcessId(tray, &pid);
    }
    if (pid != 0) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (process != nullptr) {
            TerminateProcess(process, 0);
            CloseHandle(process);
            // Give the shell a moment to exit before starting a new instance.
            Sleep(800);
        }
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    wchar_t command[] = L"explorer.exe";
    if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return true;
    }
    const INT_PTR launched =
        reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL));
    return launched > 32;
}

}  // namespace zipdesk
