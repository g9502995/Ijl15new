#pragma once
#include <windows.h>
#include <comdef.h>

// v83 Native UI Structures and Pointers
struct IWzCanvas : public IUnknown {
};

// 裝備欄位結構 (v83)
#pragma pack(push, 1)
struct GW_ItemSlotBase {
    void* vfptr;
};

struct GW_ItemSlotEquip : public GW_ItemSlotBase {
};
#pragma pack(pop)

// =============================================
// 函數指標型別定義 (基於 IDA 反組譯確認)
// =============================================

// IWzCanvas::DrawTextA -> __thiscall(this=pCanvas, nLeft, nTop, sText, pFont, &vAlpha, &vTabOrg)
typedef unsigned long (__fastcall* fnDrawTextA)(void* pCanvas, void* edx, long x, long y, void* text_ptr, void* pFont, VARIANT* vAlpha, VARIANT* vTabOrg);

// =============================================
// 函數位址
// =============================================
static const DWORD ADDR_GET_CANVAS  = 0x00425D2E;
static const DWORD ADDR_DRAW_TEXT   = 0x004277AD;

// 取得 Canvas (從 pLayer)
// 原廠呼叫 (DrawToolTip_Equip 0x8ed2df):
//   push 3     ; arg_4 = 3 (VT_I4)
//   push edi=0 ; arg_0 = 0
//   lea  ecx, pvarg
//   call sub_402FAB  → sets pvarg.vt=3, pvarg.lVal=0
//   mov  ecx, [ebx+10h]  ; ecx = pLayer
//   push &pvarg
//   push &v76
//   call GetCanvas
inline void* Layer_GetCanvas(void* pLayer) {
    if (!pLayer) return nullptr;

    // 直接複製原廠對 VARIANT 的初始化結果: vt=3(VT_I4), lVal=0
    VARIANT pvarg = {};
    pvarg.vt   = 3;    // VT_I4
    pvarg.lVal = 0;

    void* pCanvas = nullptr;
    // GetCanvas(__thiscall): ecx=pLayer, arg0=&pCanvas_out, arg4=&pvarg
    typedef void (__fastcall* fnGetCanvas)(void* pLayer, void* edx, void** pCanvas_out, VARIANT* pvarg);
    ((fnGetCanvas)ADDR_GET_CANVAS)(pLayer, nullptr, &pCanvas, &pvarg);
    return pCanvas;
}

// =============================================
// 仿照客戶端的 _bstr_t::Data_t 結構
// IDA 確認: Data_t 的 refcount 在 offset +8
// (m_wstr @ +0, m_len @ +4, m_refCnt @ +8)
// =============================================
struct GameBstrData {
    wchar_t* m_wstr;   // +0: 字串指標
    int      m_len;    // +4: 字串長度 (BSTR 慣例: 不包含 null)
    int      m_refCnt; // +8: 引用計數
};

// =============================================
// 畫文字
// 注意: DrawTextA 結束後會呼叫 Data_t::Release(sText)
//       Release 會對 m_refCnt (+8) 做 InterlockedDecrement
//       我們設 refCnt = 2，讓它 decrement 後變成 1，不觸發 delete
// =============================================
inline void Canvas_DrawTextA(void* pCanvas, long x, long y, const wchar_t* text) {
    if (!pCanvas || !text) return;

    VARIANT vAlpha, vTabOrg;
    VariantInit(&vAlpha);
    VariantInit(&vTabOrg);
    // vAlpha = 不透明白色 (根據原廠畫法，這裡傳空 variant 通常使用預設顏色)
    // 但 DrawTextA 內部呼叫的是 vptr[38] 的函式 (offset 0x98)，顏色解讀由它決定

    // 建立一個合法的 GameBstrData
    // m_wstr 要是原始 wchar_t 指標 (非 BSTR)，DrawTextA 只讀 *sText 取指標
    // *sText 就是 m_wstr，所以 sText 要指向 GameBstrData，DrawTextA 讀 *sText = m_wstr
    GameBstrData fakeData;
    fakeData.m_wstr   = SysAllocString(text); // 合法的 BSTR (有前4字節長度前綴)
    fakeData.m_len    = (int)wcslen(text);
    fakeData.m_refCnt = 2; // Release 後變 1，不觸發 delete

    // __thiscall 用 __fastcall 模擬：ecx = this，edx = nullptr (dummy)，其餘 push stack
    ((fnDrawTextA)ADDR_DRAW_TEXT)(pCanvas, nullptr, x, y, &fakeData, nullptr, &vAlpha, &vTabOrg);

    // DrawTextA 內部已經 Release 了 fakeData (refCnt 從 2 -> 1)
    // 但不會 delete，所以我們自己清理 BSTR
    if (fakeData.m_wstr) {
        SysFreeString(fakeData.m_wstr);
        fakeData.m_wstr = nullptr;
    }

    VariantClear(&vAlpha);
    VariantClear(&vTabOrg);
}
