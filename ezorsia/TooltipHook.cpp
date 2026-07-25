#define _CRT_SECURE_NO_WARNINGS
#include "stdafx.h"
#include "TooltipHook.h"
#include "SetItemPanel.h"
#include "detours.h"

// DrawToolTip_Equip: void __fastcall CUIToolTip::DrawToolTip_Equip(long nY, GW_ItemSlotEquip* pEquip)
// pThis = CUIToolTip* (which is also the pToolTip to pass to AddInfoEx)
typedef void (__fastcall* fnDrawToolTip_Equip)(void* pThis, void* edx, long nY, void* pEquip);
static fnDrawToolTip_Equip DrawToolTip_Equip_Original = (fnDrawToolTip_Equip)0x008ED0D2;

static void __fastcall Hook_DrawToolTip_Equip(void* pThis, void* edx, long nY, void* pEquip) {
    DrawToolTip_Equip_Original(pThis, edx, nY, pEquip);

    if (pEquip) {
        UpdateSetPanelData(pThis, pEquip);
    }
}

// Hook CUIToolTip::ClearToolTip (0x008E6E23) to hide our panel when tooltip goes away
typedef void (__fastcall* fnClearToolTip)(void* pThis, void* edx);
static fnClearToolTip ClearToolTip_Original = (fnClearToolTip)0x008E6E23;

static void __fastcall Hook_ClearToolTip(void* pThis, void* edx) {
    ClearToolTip_Original(pThis, edx);
    ClearSetPanelData(pThis);
}

void InitTooltipHooks() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)DrawToolTip_Equip_Original, Hook_DrawToolTip_Equip);
    DetourAttach(&(PVOID&)ClearToolTip_Original, Hook_ClearToolTip);
    DetourTransactionCommit();
}