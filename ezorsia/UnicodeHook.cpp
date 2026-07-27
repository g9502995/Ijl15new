#include "stdafx.h"
#include "UnicodeHook.h"
#include <imm.h>
#include <vector>
#include <iostream>
#include "Memory.h"

namespace UnicodeHook {

    // -----------------------------------------------------------------------
    // Helper: check if a byte buffer is valid UTF-8 AND contains at least one
    // multi-byte sequence (i.e. it is NOT pure ASCII / Big5).
    // Returns true  -> treat as UTF-8
    // Returns false -> fall back to original ANSI (Big5 / system codepage)
    // -----------------------------------------------------------------------
    static bool IsValidUTF8WithMultibyte(const char* str, int len) {
        if (!str || len <= 0) return false;

        bool hasMultibyte = false;
        int i = 0;
        while (i < len) {
            unsigned char c = (unsigned char)str[i];

            if (c < 0x80) {
                // ASCII byte — valid in both UTF-8 and Big5
                i++;
            }
            else if (c < 0xC2) {
                // 0x80-0xBF: UTF-8 continuation byte at start -> invalid UTF-8
                // 0xC0-0xC1: overlong sequence -> invalid UTF-8
                return false;
            }
            else if (c < 0xE0) {
                // 2-byte sequence
                if (i + 1 >= len) return false;
                unsigned char c2 = (unsigned char)str[i + 1];
                if ((c2 & 0xC0) != 0x80) return false;
                hasMultibyte = true;
                i += 2;
            }
            else if (c < 0xF0) {
                // 3-byte sequence (CJK lives here: U+4E00..U+9FFF etc.)
                if (i + 2 >= len) return false;
                unsigned char c2 = (unsigned char)str[i + 1];
                unsigned char c3 = (unsigned char)str[i + 2];
                if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
                hasMultibyte = true;
                i += 3;
            }
            else if (c < 0xF8) {
                // 4-byte sequence
                if (i + 3 >= len) return false;
                unsigned char c2 = (unsigned char)str[i + 1];
                unsigned char c3 = (unsigned char)str[i + 2];
                unsigned char c4 = (unsigned char)str[i + 3];
                if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80 || (c4 & 0xC0) != 0x80) return false;
                hasMultibyte = true;
                i += 4;
            }
            else {
                // 5/6-byte (invalid in modern UTF-8)
                return false;
            }
        }
        // Only treat as UTF-8 when there was at least one real multibyte sequence.
        // Pure-ASCII strings pass through the original path (no visual difference).
        return hasMultibyte;
    }

    // -----------------------------------------------------------------------
    // WideCharToMultiByte — always use CP_UTF8
    // (when game converts wchar input to char for the chat packet)
    // -----------------------------------------------------------------------
    typedef int (WINAPI* WideCharToMultiByte_t)(
        UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
        LPSTR lpMultiByteStr, int cbMultiByte, PCCH lpDefaultChar, LPBOOL lpUsedDefaultChar);
    static WideCharToMultiByte_t Real_WideCharToMultiByte = WideCharToMultiByte;

    static int WINAPI Hooked_WideCharToMultiByte(
        UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
        LPSTR lpMultiByteStr, int cbMultiByte, PCCH lpDefaultChar, LPBOOL lpUsedDefaultChar)
    {
        return Real_WideCharToMultiByte(CP_UTF8, dwFlags, lpWideCharStr, cchWideChar,
            lpMultiByteStr, cbMultiByte, lpDefaultChar, lpUsedDefaultChar);
    }

    // -----------------------------------------------------------------------
    // MultiByteToWideChar — always use CP_UTF8
    // (chat text from server/IME comes back as UTF-8)
    // -----------------------------------------------------------------------
    typedef int (WINAPI* MultiByteToWideChar_t)(
        UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte,
        LPWSTR lpWideCharStr, int cchWideChar);
    static MultiByteToWideChar_t Real_MultiByteToWideChar = MultiByteToWideChar;

    static int WINAPI Hooked_MultiByteToWideChar(
        UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte,
        LPWSTR lpWideCharStr, int cchWideChar)
    {
        return Real_MultiByteToWideChar(CP_UTF8, dwFlags, lpMultiByteStr, cbMultiByte,
            lpWideCharStr, cchWideChar);
    }

    // -----------------------------------------------------------------------
    // Passthrough helpers (declared in UnicodeHook.h) — call the *real* WinAPI
    // conversion via the trampoline Detours installed in Real_*, bypassing our
    // own Hooked_* redirection above. There is exactly one definition of these
    // (this TU), so every caller — regardless of which .cpp includes the
    // header — reaches the same Real_* pointers that ApplyHooks() actually
    // rewired to the trampoline.
    // -----------------------------------------------------------------------
    int RealMultiByteToWideChar(
        UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte,
        LPWSTR lpWideCharStr, int cchWideChar)
    {
        return Real_MultiByteToWideChar(CodePage, dwFlags, lpMultiByteStr, cbMultiByte,
            lpWideCharStr, cchWideChar);
    }

    int RealWideCharToMultiByte(
        UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
        LPSTR lpMultiByteStr, int cbMultiByte, PCCH lpDefaultChar, LPBOOL lpUsedDefaultChar)
    {
        return Real_WideCharToMultiByte(CodePage, dwFlags, lpWideCharStr, cchWideChar,
            lpMultiByteStr, cbMultiByte, lpDefaultChar, lpUsedDefaultChar);
    }

    // -----------------------------------------------------------------------
    // GBK (CP936) -> UTF-8 conversion helper
    // Used by StringPool hook to convert GBK source-embedded strings to UTF-8
    // so our TextOutA hook can detect and render them via TextOutW.
    // -----------------------------------------------------------------------
    std::string GbkToUtf8(const std::string& gbkStr) {
        if (gbkStr.empty()) return gbkStr;
        int wlen = RealMultiByteToWideChar(936, 0, gbkStr.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return gbkStr;
        std::wstring wstr(wlen, L'\0');
        RealMultiByteToWideChar(936, 0, gbkStr.c_str(), -1, &wstr[0], wlen);
        int ulen = RealWideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (ulen <= 0) return gbkStr;
        std::string utf8(ulen, '\0');
        RealWideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8[0], ulen, nullptr, nullptr);
        if (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();
        return utf8;
    }

    // -----------------------------------------------------------------------
    // CharNextA — UTF-8 aware (skip continuation bytes)
    // -----------------------------------------------------------------------
    typedef LPSTR(WINAPI* CharNextA_t)(LPCSTR lpCurrentChar);
    static CharNextA_t Real_CharNextA = CharNextA;

    static LPSTR WINAPI Hooked_CharNextA(LPCSTR lpCurrentChar) {
        LPCSTR result = lpCurrentChar;
        if (*result) {
            unsigned char c;
            do {
                c = (unsigned char)*++result;
            } while ((c & 0xC0) == 0x80);
        }
        return (LPSTR)result;
    }

    // -----------------------------------------------------------------------
    // CharPrevA — UTF-8 aware (walk back over continuation bytes)
    // -----------------------------------------------------------------------
    typedef LPSTR(WINAPI* CharPrevA_t)(LPCSTR lpszStart, LPCSTR lpszCurrent);
    static CharPrevA_t Real_CharPrevA = CharPrevA;

    static LPSTR WINAPI Hooked_CharPrevA(LPCSTR lpszStart, LPCSTR lpszCurrent) {
        LPCSTR result = lpszCurrent;
        unsigned char c;
        do {
            if (result <= lpszStart) break;
            c = (unsigned char)*--result;
        } while ((c & 0xC0) == 0x80);
        return (LPSTR)result;
    }

    // -----------------------------------------------------------------------
    // Helper: get/cache a Unicode-capable HFONT matching the DC's current font
    // metrics (same size & weight), so CJK chars render at the right scale.
    // We prefer Microsoft YaHei → SimSun → MingLiU as fallbacks.
    // -----------------------------------------------------------------------
    static HFONT GetUnicodeFont(HDC hdc) {
        // Read current font metrics so our Unicode font matches the size
        LOGFONTW lf = {};
        HFONT hCur = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
        GetObjectW(hCur, sizeof(lf), &lf);

        // Override charset & face to a Unicode-capable font
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;

        // Try Microsoft YaHei first (has full CJK Unified Ideographs)
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei");
        HFONT hFont = CreateFontIndirectW(&lf);
        if (!hFont) {
            wcscpy_s(lf.lfFaceName, L"SimSun");
            hFont = CreateFontIndirectW(&lf);
        }
        if (!hFont) {
            wcscpy_s(lf.lfFaceName, L"MingLiU");
            hFont = CreateFontIndirectW(&lf);
        }
        return hFont; // caller must DeleteObject
    }

    // -----------------------------------------------------------------------
    // TextOutA — smart: UTF-8 → Unicode font → TextOutW, else original Big5
    // -----------------------------------------------------------------------
    typedef BOOL(WINAPI* TextOutA_t)(HDC hdc, int x, int y, LPCSTR lpString, int c);
    static TextOutA_t Real_TextOutA = TextOutA;

    static BOOL WINAPI Hooked_TextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c) {
        if (!lpString || c <= 0) return Real_TextOutA(hdc, x, y, lpString, c);

        if (IsValidUTF8WithMultibyte(lpString, c)) {
            std::vector<wchar_t> wideBuf(c * 2 + 2);
            int len = RealMultiByteToWideChar(CP_UTF8, 0, lpString, c, wideBuf.data(), (int)wideBuf.size() - 1);
            if (len > 0) {
                wideBuf[len] = L'\0';
                // Temporarily swap to Unicode font so glyphs are available
                HFONT hUniFont = GetUnicodeFont(hdc);
                HFONT hOldFont = hUniFont ? (HFONT)SelectObject(hdc, hUniFont) : nullptr;
                BOOL result = TextOutW(hdc, x, y, wideBuf.data(), len);
                if (hOldFont) SelectObject(hdc, hOldFont);
                if (hUniFont) DeleteObject(hUniFont);
                return result;
            }
        }

        // Big5 / ASCII game strings → pass through unchanged
        return Real_TextOutA(hdc, x, y, lpString, c);
    }

    // -----------------------------------------------------------------------
    // GetTextExtentPoint32A — same smart detection + Unicode font swap
    // -----------------------------------------------------------------------
    typedef BOOL(WINAPI* GetTextExtentPoint32A_t)(HDC hdc, LPCSTR lpString, int c, LPSIZE psizl);
    static GetTextExtentPoint32A_t Real_GetTextExtentPoint32A = GetTextExtentPoint32A;

    static BOOL WINAPI Hooked_GetTextExtentPoint32A(HDC hdc, LPCSTR lpString, int c, LPSIZE psizl) {
        if (!lpString || c <= 0) return Real_GetTextExtentPoint32A(hdc, lpString, c, psizl);

        if (IsValidUTF8WithMultibyte(lpString, c)) {
            std::vector<wchar_t> wideBuf(c * 2 + 2);
            int len = RealMultiByteToWideChar(CP_UTF8, 0, lpString, c, wideBuf.data(), (int)wideBuf.size() - 1);
            if (len > 0) {
                wideBuf[len] = L'\0';
                HFONT hUniFont = GetUnicodeFont(hdc);
                HFONT hOldFont = hUniFont ? (HFONT)SelectObject(hdc, hUniFont) : nullptr;
                BOOL result = GetTextExtentPoint32W(hdc, wideBuf.data(), len, psizl);
                if (hOldFont) SelectObject(hdc, hOldFont);
                if (hUniFont) DeleteObject(hUniFont);
                return result;
            }
        }

        return Real_GetTextExtentPoint32A(hdc, lpString, c, psizl);
    }

    // -----------------------------------------------------------------------
    // ImmAssociateContext — force IME open on every window
    // -----------------------------------------------------------------------
    typedef HIMC(WINAPI* ImmAssociateContext_t)(HWND hWnd, HIMC hIMC);
    static ImmAssociateContext_t Real_ImmAssociateContext = ImmAssociateContext;

    static HIMC WINAPI Hooked_ImmAssociateContext(HWND hWnd, HIMC hIMC) {
        HIMC overrideHIMC = ImmGetContext(hWnd);
        ImmSetOpenStatus(overrideHIMC, TRUE);
        return Real_ImmAssociateContext(hWnd, overrideHIMC);
    }

    // -----------------------------------------------------------------------
    // PeekMessageA → PeekMessageW (receive WM_CHAR as wide char)
    // -----------------------------------------------------------------------
    typedef BOOL(WINAPI* PeekMessageA_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
    static PeekMessageA_t Real_PeekMessageA = PeekMessageA;

    static BOOL WINAPI Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
        return PeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    }

    // -----------------------------------------------------------------------
    // Shared Unicode UI font for window/control captions (SetWindowTextA hook
    // below). Unlike GetUnicodeFont(hdc), this isn't matched to a specific DC's
    // current metrics — it's created once and kept alive for the process
    // lifetime, since it gets attached to a window via WM_SETFONT and must
    // outlive the SetWindowTextA call that assigned it.
    // -----------------------------------------------------------------------
    static HFONT GetSharedUnicodeUIFont() {
        static HFONT hFont = nullptr;
        if (!hFont) {
            LOGFONTW lf = {};
            lf.lfHeight = -14;
            lf.lfWeight = FW_NORMAL;
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(lf.lfFaceName, L"Microsoft YaHei");
            hFont = CreateFontIndirectW(&lf);
            if (!hFont) {
                wcscpy_s(lf.lfFaceName, L"SimSun");
                hFont = CreateFontIndirectW(&lf);
            }
            if (!hFont) {
                wcscpy_s(lf.lfFaceName, L"MingLiU");
                hFont = CreateFontIndirectW(&lf);
            }
        }
        return hFont;
    }

    // -----------------------------------------------------------------------
    // Identify the game's main top-level window: the first parent-less,
    // owner-less, WS_CAPTION window belonging to this process that we see go
    // through SetWindowTextA. Cached after the first match so every
    // subsequent hit is a cheap HWND comparison.
    // -----------------------------------------------------------------------
    static HWND g_hMainWnd = nullptr;

    static bool IsMainGameWindow(HWND hWnd) {
        if (g_hMainWnd) return hWnd == g_hMainWnd;

        if (GetParent(hWnd) != nullptr) return false;
        if (GetWindow(hWnd, GW_OWNER) != nullptr) return false;

        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid != GetCurrentProcessId()) return false;

        LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        if (!(style & WS_CAPTION)) return false;

        g_hMainWnd = hWnd;
        return true;
    }

    // -----------------------------------------------------------------------
    // SetWindowTextA — smart: UTF-8 → Unicode font + SetWindowTextW, else
    // original Big5/ANSI passthrough. Handles window/control captions (title
    // bars, buttons, static labels, list items, etc.), which are set via
    // USER32's WM_SETTEXT mechanism and never touch GDI TextOutA at all, so
    // the TextOutA hook above can't catch these on its own.
    //
    // Also hijacks the main game window's title bar to a fixed string,
    // regardless of whatever (garbled Simplified Chinese) text the client
    // tries to set — every SetWindowTextA call aimed at that one window is
    // overridden, so it can't be reset back to the broken text later either.
    // -----------------------------------------------------------------------
    typedef BOOL(WINAPI* SetWindowTextA_t)(HWND hWnd, LPCSTR lpString);
    static SetWindowTextA_t Real_SetWindowTextA = SetWindowTextA;

    static BOOL WINAPI Hooked_SetWindowTextA(HWND hWnd, LPCSTR lpString) {
        if (!lpString) return Real_SetWindowTextA(hWnd, lpString);

        int len = (int)strlen(lpString);
        if (len > 0 && IsValidUTF8WithMultibyte(lpString, len)) {
            std::vector<wchar_t> wideBuf(len * 2 + 2);
            int wlen = RealMultiByteToWideChar(CP_UTF8, 0, lpString, len, wideBuf.data(), (int)wideBuf.size() - 1);
            if (wlen > 0) {
                wideBuf[wlen] = L'\0';
                HFONT hUniFont = GetSharedUnicodeUIFont();
                if (hUniFont) {
                    SendMessageW(hWnd, WM_SETFONT, (WPARAM)hUniFont, TRUE);
                }
                return SetWindowTextW(hWnd, wideBuf.data());
            }
        }

        return Real_SetWindowTextA(hWnd, lpString);
    }

    // -----------------------------------------------------------------------
    // Apply all hooks
    // -----------------------------------------------------------------------
    bool ApplyHooks() {
        bool bResult = true;
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_WideCharToMultiByte), Hooked_WideCharToMultiByte);
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_MultiByteToWideChar), Hooked_MultiByteToWideChar);
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_CharNextA), Hooked_CharNextA);
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_CharPrevA), Hooked_CharPrevA);
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_TextOutA), Hooked_TextOutA);
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_GetTextExtentPoint32A), Hooked_GetTextExtentPoint32A);
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_ImmAssociateContext), Hooked_ImmAssociateContext);
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_PeekMessageA), Hooked_PeekMessageA);
        bResult &= Memory::SetHook(true, reinterpret_cast<void**>(&Real_SetWindowTextA), Hooked_SetWindowTextA);
        std::cout << "[UnicodeHook] Win32 Unicode IME Hooks initialized: " << (bResult ? "SUCCESS" : "PARTIAL") << "\n";
        return bResult;
    }
}
