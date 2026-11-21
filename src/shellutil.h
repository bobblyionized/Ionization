#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <initializer_list>

// Shell helpers for the dock: which windows are "apps", their identity, icons and launch info.
namespace ShellUtil {
    bool IsAppWindow(HWND h);                       // what the taskbar would show
    std::wstring WindowAumid(HWND h);               // explicit AppUserModelID, if any
    std::wstring WindowExe(HWND h);                 // full exe path
    std::wstring Lower(std::wstring s);
    struct Link { std::wstring path, target, aumid, name; };
    bool ResolveLink(const std::wstring& lnk, Link& out);
    std::vector<std::wstring> TaskbarPins();        // .lnk files pinned to the Windows taskbar
    std::wstring DisplayName(const std::wstring& parsingName);
    std::wstring FileDescription(const std::wstring& exe);
    HBITMAP Icon(const std::wstring& parsingName, int size);   // caller DeleteObject()s
    void Launch(const std::wstring& parsingName);
    void SendKeys(std::initializer_list<WORD> vks);  // e.g. { VK_LWIN, 'A' }
    // Desktop icons: the same switch as right-click desktop > View > Show desktop icons.
    bool DesktopIconsVisible();
    bool SetDesktopIcons(bool visible);
}
