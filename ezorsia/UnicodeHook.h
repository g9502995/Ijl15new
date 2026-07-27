#pragma once
#include <windows.h>
#include <string>

#pragma comment(lib, "imm32.lib")

namespace UnicodeHook {

    // Installs all Win32-level Unicode/IME hooks (see UnicodeHook.cpp). Call once
    // from DllMain after other hooks are set up.
    bool ApplyHooks();

    // GBK (CP936) -> UTF-8 conversion helper.
    // Used by StringPool-style hooks to convert GBK source-embedded strings to
    // UTF-8 so the TextOutA hook can detect and render them via TextOutW.
    std::string GbkToUtf8(const std::string& gbkStr);

    // -----------------------------------------------------------------------
    // Passthrough helpers — call the *real* WinAPI conversion, bypassing our
    // own Hooked_* redirection in UnicodeHook.cpp. Once ApplyHooks() is
    // active, Detours patches MultiByteToWideChar/WideCharToMultiByte
    // in-process, so ANY direct call to those symbols (including from our own
    // DLL, from any translation unit) gets forced to CP_UTF8. Any of our own
    // code that needs a specific non-UTF8 codepage (e.g. CP_ACP / Big5, or
    // GBK 936) must go through these instead.
    // -----------------------------------------------------------------------
    int RealMultiByteToWideChar(
        UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte,
        LPWSTR lpWideCharStr, int cchWideChar);

    int RealWideCharToMultiByte(
        UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
        LPSTR lpMultiByteStr, int cbMultiByte, PCCH lpDefaultChar, LPBOOL lpUsedDefaultChar);
}
