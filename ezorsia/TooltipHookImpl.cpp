// TooltipHookImpl.cpp
// 此檔案必須以 /EHsc 編譯 (專案設定中已啟用 C++ Exception Handling)
#include "stdafx.h"
#include "NativeUI.h"

// GetCanvas 的安全包裝 - 以 try/catch 攔截 _com_error
void* SafeGetCanvas_Impl(void* pLayer) {
    void* pCanvas = nullptr;
    if (!pLayer) return nullptr;
    try {
        VARIANT pvarg;
        memset(&pvarg, 0, sizeof(pvarg));
        pvarg.vt   = 3;    // VT_I4 (原廠使用的格式)
        pvarg.lVal = 0;
        typedef void (__fastcall* fnGetCanvas)(void* pLayer, void* edx, void** pCanvas_out, VARIANT* pvarg);
        ((fnGetCanvas)0x00425D2E)(pLayer, nullptr, &pCanvas, &pvarg);
    } catch (...) {
        pCanvas = nullptr;
    }
    return pCanvas;
}

// DrawTextA 的安全包裝
void SafeDrawTextA_Impl(void* pCanvas, long x, long y, const wchar_t* text) {
    if (!pCanvas || !text) return;
    try {
        Canvas_DrawTextA(pCanvas, x, y, text);
    } catch (...) {}
}

// Release 的安全包裝
void SafeRelease_Impl(void* pCanvas) {
    if (!pCanvas) return;
    try {
        ((IWzCanvas*)pCanvas)->Release();
    } catch (...) {}
}