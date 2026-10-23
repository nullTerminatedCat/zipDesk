#pragma once

namespace zipdesk {

// Windows 11 build 22000+ only. Registry changes are HKCU-only (no admin).
bool isWindows11ForClassicMenu();
bool isClassicContextMenuEnabled();
bool enableClassicContextMenu();
bool disableClassicContextMenu();
// Ends the shell tray process and starts explorer.exe again.
bool restartWindowsExplorer();

}  // namespace zipdesk
