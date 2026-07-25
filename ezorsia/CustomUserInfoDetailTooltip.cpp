#include "stdafx.h"
#include "CustomUserInfoDetailTooltip.h"
#include "Memory.h"
#include "detours.h"

// ===========================================================================
//  Shows a target character's server-verified real equip stats when hovering
//  equipment rows in the character info detail panel (CUIUserInfoDetail).
//
//  Earlier attempts called CUIToolTip::ClearToolTip (0x8E6E23) to hide the
//  tooltip, which never worked. Decompiling CUIToolTip's real destructor
//  (0x8E6BA3) shows why: ClearToolTip only resets content - the destructor
//  additionally releases a whole set of _com_ptr_t<IWzGr2DLayer> D3D canvas
//  layers that ShowItemToolTip (0x8F5B20) creates via MakeLayer. Those layers
//  are what the engine's layer compositor actually draws every frame,
//  independent of whether CUIUserInfoDetail's own Draw() ever runs again -
//  Clear() alone never releases them, so the last shown layer just keeps
//  compositing forever. The fix is to fully destruct (release the layers)
//  and reconstruct the tooltip object on hide, not merely clear it.
// ===========================================================================

// ---------------------------------------------------------------------------
//  GW_ItemSlotEquip stat fields
//
//  Each stat is stored as an anti-tamper pair of DWORDs (value-slot, then
//  key-slot immediately after) inside the item instance, confirmed by
//  decompiling CUIToolTip::SetToolTip_Equip_Basic (0x8ECAFA)'s per-stat
//  getters - e.g. STR reads as _ZtlSecureFuse<short>(this+13, this[14]).
//
//  The matching WRITE side is CItemInfo::GetItemSlot (0x5D5D95) itself: when
//  it builds a fresh GW_ItemSlotEquip from an item's template stats, it does
//  `v26[14] = sub_4E80EB(templateSTR, v26 + 13)` - i.e. call sub_4E80EB with
//  the new value and a pointer to the value-slot, then store the return into
//  the key-slot right after it. We reuse that exact real call to overwrite
//  with server-verified stats instead of guessing the encoding ourselves.
// ---------------------------------------------------------------------------
typedef unsigned int(__fastcall* PFN_EncodeStatField)(short value, int* pValueSlot);
static const PFN_EncodeStatField g_EncodeStatField = (PFN_EncodeStatField)0x004E80EB;

enum EquipStatSlot {
    SLOT_STR = 13, SLOT_DEX = 15, SLOT_INT = 17, SLOT_LUK = 19,
    SLOT_HP = 21, SLOT_MP = 23, SLOT_PAD = 25, SLOT_MAD = 27,
    SLOT_PDD = 29, SLOT_MDD = 31, SLOT_ACC = 33, SLOT_EVA = 35,
    SLOT_CRAFT = 37, SLOT_SPEED = 39, SLOT_JUMP = 41,
};

static void WriteEquipStat(int* pEquip, int valueSlot, short value) {
    if (!pEquip || (uintptr_t)pEquip < 0x10000) return;
    __try {
        pEquip[valueSlot + 1] = (int)g_EncodeStatField(value, pEquip + valueSlot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
//  Real-stat packet: SEND_RECV_USERINFOEX (0x1238), confirmed against the
//  server source (UserInfoExHandler.java / PacketCreator.getUserInfoEx) -
//  field order and types below match the server's writes byte for byte.
// ---------------------------------------------------------------------------
static const unsigned short SEND_RECV_USERINFOEX = 0x1238;

typedef void(__thiscall* PFN_COutPacket_ctor)(void* pThis, int nOpcode);
typedef void(__thiscall* PFN_Encode1)(void* pThis, unsigned char value);
typedef void(__thiscall* PFN_Encode4)(void* pThis, unsigned int value);
typedef void(__thiscall* PFN_SendPacket)(void* pSocket, void* pPacket);
typedef unsigned char(__thiscall* PFN_Decode1)(void* pPacket);
typedef unsigned short(__thiscall* PFN_Decode2)(void* pPacket);
typedef unsigned int(__thiscall* PFN_Decode4)(void* pPacket);

static const PFN_COutPacket_ctor g_COutPacket_ctor = (PFN_COutPacket_ctor)0x006EC9CE;
static const PFN_Encode1 g_Encode1 = (PFN_Encode1)0x0040654E;
static const PFN_Encode4 g_Encode4 = (PFN_Encode4)0x004065A6;
static const PFN_SendPacket g_SendPacket = (PFN_SendPacket)0x0049637B;
static const PFN_Decode1 g_Decode1 = (PFN_Decode1)0x004065F3;
static const PFN_Decode2 g_Decode2 = (PFN_Decode2)0x0042470C;
static const PFN_Decode4 g_Decode4 = (PFN_Decode4)0x00406629;
static void*** const g_pClientSocketPtr = (void***)0x00BE7914;

struct RealEquipStats {
    int targetCharID = -1;
    int itemID = 0;
    short str = 0, dex = 0, intt = 0, luk = 0, hp = 0, mp = 0;
    short pad = 0, mad = 0, pdd = 0, mdd = 0, acc = 0, eva = 0;
    short craft = 0, speed = 0, jump = 0;
    bool  valid = false;

    void clear() { *this = RealEquipStats(); }
};
static RealEquipStats g_realStats;

static void SendUserInfoExRequest(int targetCharID, int itemID)
{
    if (!g_pClientSocketPtr || !*g_pClientSocketPtr || !**g_pClientSocketPtr) return;
    __try {
        char oPacket[0x40] = { 0 };
        g_COutPacket_ctor(oPacket, SEND_RECV_USERINFOEX);
        g_Encode1(oPacket, 1); // type = 1: request a target character's real equip stats
        g_Encode4(oPacket, static_cast<unsigned int>(targetCharID));
        g_Encode4(oPacket, static_cast<unsigned int>(itemID));
        g_SendPacket(*g_pClientSocketPtr, oPacket);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

typedef void(__fastcall* PFN_OnPacket)(void* pThis, void* edx, void* pPacket);
static PFN_OnPacket g_OnPacket_Original = (PFN_OnPacket)0x004965F1;

static void StoreRealStats(const RealEquipStats& s) { g_realStats = s; }

static void __fastcall Hooked_OnPacket_UserInfoEx(void* pThis, void* edx, void* pPacket)
{
    __try {
        if (!pPacket) {
            g_OnPacket_Original(pThis, edx, pPacket);
            return;
        }

        unsigned short* pLen = reinterpret_cast<unsigned short*>(reinterpret_cast<uintptr_t>(pPacket) + 0x0C);
        unsigned int* pReadPos = reinterpret_cast<unsigned int*>(reinterpret_cast<uintptr_t>(pPacket) + 0x14);

        if (pLen && pReadPos && (*pLen >= *pReadPos + 2)) {
            const unsigned int savedPos = *pReadPos;
            const unsigned short opcode = g_Decode2(pPacket);

            if (opcode == SEND_RECV_USERINFOEX) {
                if (*pLen >= *pReadPos + 1 + 4 + 4 + 4 + 30 + 1) {
                    const unsigned char type = g_Decode1(pPacket);
                    if (type == 1) {
                        RealEquipStats s;
                        s.targetCharID = static_cast<int>(g_Decode4(pPacket));
                        s.itemID       = static_cast<int>(g_Decode4(pPacket));
                        g_Decode4(pPacket); // nAnvilItemID
                        s.str   = static_cast<short>(g_Decode2(pPacket));
                        s.dex   = static_cast<short>(g_Decode2(pPacket));
                        s.intt  = static_cast<short>(g_Decode2(pPacket));
                        s.luk   = static_cast<short>(g_Decode2(pPacket));
                        s.hp    = static_cast<short>(g_Decode2(pPacket));
                        s.mp    = static_cast<short>(g_Decode2(pPacket));
                        s.pad   = static_cast<short>(g_Decode2(pPacket));
                        s.mad   = static_cast<short>(g_Decode2(pPacket));
                        s.pdd   = static_cast<short>(g_Decode2(pPacket));
                        s.mdd   = static_cast<short>(g_Decode2(pPacket));
                        s.acc   = static_cast<short>(g_Decode2(pPacket));
                        s.eva   = static_cast<short>(g_Decode2(pPacket));
                        s.craft = static_cast<short>(g_Decode2(pPacket));
                        s.speed = static_cast<short>(g_Decode2(pPacket));
                        s.jump  = static_cast<short>(g_Decode2(pPacket));
                        g_Decode1(pPacket); // upgradeSlots
                        s.valid = true;
                        StoreRealStats(s);
                    }
                    return;
                }
            }
            *pReadPos = savedPos;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    g_OnPacket_Original(pThis, edx, pPacket);
}

typedef void*(__thiscall* PFN_CItemInfo_GetItemSlot)(void* pThis, void* pZRefOut, int itemId);
static const PFN_CItemInfo_GetItemSlot g_GetItemSlot = (PFN_CItemInfo_GetItemSlot)0x005D5D95;
static void* const g_pCItemInfoInstance = reinterpret_cast<void*>(0x00BE78D8); // &CItemInfo singleton ptr

typedef void(__thiscall* PFN_ItemToolTipParam_ctor)(void* pThis);
static const PFN_ItemToolTipParam_ctor g_ItemToolTipParam_ctor = (PFN_ItemToolTipParam_ctor)0x00483EED;
// Confirmed via disasm: retn (bare) - 0 stack args, just `this`. (An earlier
// version of this file guessed a "scalar deleting destructor" flags param
// here based on general MSVC convention without checking - that was wrong
// and pushed an extra dword the callee never popped.)
typedef void(__thiscall* PFN_ItemToolTipParam_dtor)(void* pThis);
static const PFN_ItemToolTipParam_dtor g_ItemToolTipParam_dtor = (PFN_ItemToolTipParam_dtor)0x00483F18;

typedef int(__thiscall* PFN_GetDataLong)(void* pThis);
static const PFN_GetDataLong g_TSecTypeGetData = (PFN_GetDataLong)0x0042873D;

typedef void*(__thiscall* PFN_GetScrollCtrl)(void* pThis);
static const PFN_GetScrollCtrl g_GetScrollCtrl = (PFN_GetScrollCtrl)0x0064BCCC;
typedef int(__thiscall* PFN_GetScrollOffset)(void* pThis);
static const PFN_GetScrollOffset g_GetScrollOffset = (PFN_GetScrollOffset)0x0064B1A1;

typedef void(__thiscall* PFN_CUIToolTip_ctor)(void* pThis);
static const PFN_CUIToolTip_ctor g_CUIToolTip_ctor = (PFN_CUIToolTip_ctor)0x008E49B5;
// Full destructor, NOT ClearToolTip - releases the IWzGr2DLayer canvases
// ShowItemToolTip creates. retn (bare), 0 stack args, confirmed via disasm.
typedef void(__thiscall* PFN_CUIToolTip_dtor)(void* pThis);
static const PFN_CUIToolTip_dtor g_CUIToolTip_dtor = (PFN_CUIToolTip_dtor)0x008E6BA3;
typedef void(__thiscall* PFN_CUIToolTip_Clear)(void* pThis);
static const PFN_CUIToolTip_Clear g_CUIToolTip_Clear = (PFN_CUIToolTip_Clear)0x008E6E23;
typedef void(__thiscall* PFN_CUIToolTip_Show)(void* pThis, int x, int y, void* pItemSlot, void* pParam, int a6, int a7, int a8, unsigned int a9);
static const PFN_CUIToolTip_Show g_CUIToolTip_Show = (PFN_CUIToolTip_Show)0x008F5B20;

// GetItemSlot hands back a refcounted ZRef<GW_ItemSlotBase> (it
// InterlockedIncrement's the slot's own refcount at +4 before returning) -
// every call needs a matching release or the item slot never gets freed.
// retn 4 - scalar deleting destructor, pass flags=0 ("release the ref, don't
// operator-delete `this`" - `this` here is our own stack buffer).
typedef LONG(__thiscall* PFN_ZRefItemSlot_dtor)(void* pThis, int flags);
static const PFN_ZRefItemSlot_dtor g_ZRefItemSlot_dtor = (PFN_ZRefItemSlot_dtor)0x00428A50;

static char g_tooltipMem[0x280] = {};
static bool g_bToolTipInit = false;

static bool  g_bIfCreate = false;
static int   g_nLastEquipRow = -1;
static bool  g_bCustomToolTipShowing = false;
static int   g_nHoverX = 0, g_nHoverY = 0;
static int   g_nHoverItemID = 0;
static int   g_nTargetCharID = -1;
static bool  g_bAppliedRealStats = false;
static DWORD g_nLastHoverTick = 0;   // updated every frame we confirm hover

// Fully tear down the tooltip (releasing its D3D layers) rather than just
// clearing its content - see the header comment for why ClearToolTip alone
// never actually made it disappear.
static void DestroyToolTip()
{
    if (!g_bToolTipInit) return;
    __try {
        g_CUIToolTip_dtor(g_tooltipMem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_bToolTipInit = false;
}

static void ResetHoverState()
{
    g_nLastEquipRow    = -1;
    g_bCustomToolTipShowing = false;
    g_nHoverItemID     = 0;
    g_nTargetCharID    = -1;
    g_bAppliedRealStats = false;
    g_nLastHoverTick   = 0;
    DestroyToolTip();
}

static void ShowRealStatTooltip(int itemID, int hoverX, int hoverY, int targetCharID)
{
    if (!g_pCItemInfoInstance || !*reinterpret_cast<void**>(g_pCItemInfoInstance)) return;

    unsigned char zrefBuf[8] = {};
    g_GetItemSlot(*reinterpret_cast<void**>(g_pCItemInfoInstance), zrefBuf, itemID);
    void* pItemSlot = *reinterpret_cast<void**>(zrefBuf + 4);

    if (!pItemSlot || (uintptr_t)pItemSlot < 0x10000) return;

    if (g_realStats.valid && itemID == g_realStats.itemID &&
        targetCharID == g_realStats.targetCharID) {
        int* pEquip = reinterpret_cast<int*>(pItemSlot);
        WriteEquipStat(pEquip, SLOT_STR, g_realStats.str);
        WriteEquipStat(pEquip, SLOT_DEX, g_realStats.dex);
        WriteEquipStat(pEquip, SLOT_INT, g_realStats.intt);
        WriteEquipStat(pEquip, SLOT_LUK, g_realStats.luk);
        WriteEquipStat(pEquip, SLOT_HP, g_realStats.hp);
        WriteEquipStat(pEquip, SLOT_MP, g_realStats.mp);
        WriteEquipStat(pEquip, SLOT_PAD, g_realStats.pad);
        WriteEquipStat(pEquip, SLOT_MAD, g_realStats.mad);
        WriteEquipStat(pEquip, SLOT_PDD, g_realStats.pdd);
        WriteEquipStat(pEquip, SLOT_MDD, g_realStats.mdd);
        WriteEquipStat(pEquip, SLOT_ACC, g_realStats.acc);
        WriteEquipStat(pEquip, SLOT_EVA, g_realStats.eva);
        WriteEquipStat(pEquip, SLOT_CRAFT, g_realStats.craft);
        WriteEquipStat(pEquip, SLOT_SPEED, g_realStats.speed);
        WriteEquipStat(pEquip, SLOT_JUMP, g_realStats.jump);
    }

    bool freshCtor = !g_bToolTipInit;
    if (!g_bToolTipInit) {
        memset(g_tooltipMem, 0, sizeof(g_tooltipMem));
        __try { g_CUIToolTip_ctor(g_tooltipMem); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_ZRefItemSlot_dtor(zrefBuf, 0); return; }
        g_bToolTipInit = true;
    }
    __try { g_CUIToolTip_Clear(g_tooltipMem); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    unsigned char paramBuf[0x2C] = {};
    g_ItemToolTipParam_ctor(paramBuf);
    __try {
        g_CUIToolTip_Show(g_tooltipMem, hoverX, hoverY, pItemSlot, paramBuf, 0, 1, 0, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ItemToolTipParam_dtor(paramBuf);

    g_ZRefItemSlot_dtor(zrefBuf, 0);
}

typedef void*(__thiscall* PFN_Draw)(void* pThis, int arg0);
static PFN_Draw g_Draw_Original = (PFN_Draw)0x008FD4C0;

static void* __fastcall Hooked_Draw(void* pThis, void* edx, int arg0)
{
    void* ret = NULL;
    __try {
        ret = g_Draw_Original(pThis, arg0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }

    __try {
        if (g_bCustomToolTipShowing && g_nHoverItemID != 0) {
            // Auto-hide: if OnMouseMove has not confirmed a hover in the last
            // 150 ms the mouse has moved away (or to a window that doesn't
            // fire this hook), so tear the tooltip down now.
            if (GetTickCount() - g_nLastHoverTick > 150) {
                ResetHoverState();
            } else {
                // Re-show every frame so the tooltip survives being covered
                // by other UI layers drawn after us.
                ShowRealStatTooltip(g_nHoverItemID, g_nHoverX, g_nHoverY, g_nTargetCharID);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return ret;
}

typedef void(__thiscall* PFN_DestructorOnCreate)(void* pThis);
static PFN_DestructorOnCreate g_DestructorOnCreate_Original = (PFN_DestructorOnCreate)0x008FF36F;

static void __fastcall Hooked_DestructorOnCreate(void* pThis, void* edx)
{
    __try {
        ResetHoverState();
        g_realStats.clear();
        g_bIfCreate = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    g_DestructorOnCreate_Original(pThis);
}

typedef int(__thiscall* PFN_CWnd_OnMouseMove)(void* pThis, int x, int y);
static PFN_CWnd_OnMouseMove g_OnMouseMove_Original = (PFN_CWnd_OnMouseMove)0x00424446;

static int __fastcall Hooked_OnMouseMove(void* pThis, void* edx, int x, int y)
{
    int ret = 0;
    __try {
        ret = g_OnMouseMove_Original(pThis, x, y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    __try {
        g_bIfCreate = true;

        if (!pThis || (uintptr_t)pThis < 0x10000) goto done;

        DWORD dwCtx = *reinterpret_cast<DWORD*>(0x00BEDCCC);
        if (!dwCtx || dwCtx < 0x10000) goto done;

        BYTE* pEquipArray = *reinterpret_cast<BYTE**>(dwCtx + 1796);
        if (!pEquipArray || (uintptr_t)pEquipArray < 0x10000) goto done;

        const int equipCount = *reinterpret_cast<int*>(pEquipArray - 4);
        if (equipCount <= 0 || equipCount > 500) goto done;

        const int targetCharID = *reinterpret_cast<DWORD*>(dwCtx + 1636);

        // pThis is a CWnd-derived child inside the panel; pThis+4 is the
        // CUIUserInfoDetail host that owns the scroll control (confirmed
        // live: *(pThis+4) == 0xB3BE9C, CUIUserInfoDetail's real vtable).
        //
        // The hook point (0x424446) is CWnd's own base OnMouseMove, which
        // CUIUserInfoDetail never overrides - so this fires for every other
        // CWnd-derived control in the game that also doesn't override it, not
        // just this panel. The vtable check filters those out; without it,
        // hovering some unrelated control whose local (x,y) happens to fall
        // in a row's hit box would keep the tooltip "showing" forever.
        void* pDetail = reinterpret_cast<char*>(pThis) + 4;
        if (!pDetail || (uintptr_t)pDetail < 0x10000) goto done;
        if (*reinterpret_cast<void**>(pDetail) != reinterpret_cast<void*>(0xB3BE9C)) goto done;

        int scrollOffset = 0;
        {
            void* pScrollCtrl = g_GetScrollCtrl(reinterpret_cast<char*>(pDetail) + 0x74);
            if (pScrollCtrl && (uintptr_t)pScrollCtrl >= 0x10000) {
                scrollOffset = g_GetScrollOffset(pScrollCtrl);
            }
        }

        void** vtable = *reinterpret_cast<void***>(pThis);
        if (!vtable || (uintptr_t)vtable < 0x10000) goto done;

        auto getAbsLeft = reinterpret_cast<int(__thiscall*)(void*)>(vtable[11]);
        auto getAbsTop  = reinterpret_cast<int(__thiscall*)(void*)>(vtable[12]);
        if (!getAbsLeft || !getAbsTop) goto done;

        const int nAbsLeft = getAbsLeft(pThis);
        const int nAbsTop  = getAbsTop(pThis);

        for (int k = 0; k < 3; k++) {
            const int yTop = 40 * k + 32;
            if (x < 17 || x > 250 || y < yTop || y > yTop + 32) continue;

            const int realRow = scrollOffset + k;
            if (realRow < 0 || realRow >= equipCount) goto done;

            const int itemID = g_TSecTypeGetData(pEquipArray + 12 * realRow);
            if (itemID == 0) goto done;

            const bool isNewRow = (g_nLastEquipRow != realRow || g_nTargetCharID != targetCharID);
            if (isNewRow) {
                g_nLastEquipRow = realRow;
                g_realStats.clear();
                g_bAppliedRealStats = false;
                SendUserInfoExRequest(targetCharID, itemID);
            }

            g_nTargetCharID = targetCharID;
            g_nHoverX = nAbsLeft + x + 20;
            g_nHoverY = nAbsTop + y + 20;
            g_nHoverItemID = itemID;
            g_bCustomToolTipShowing = true;
            g_nLastHoverTick = GetTickCount();  // refresh the timeout each hover frame
            return ret;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

done:
    ResetHoverState();
    return ret;
}

void AttachUIUserInfoDetail()
{
    Memory::SetHook(true, reinterpret_cast<void**>(&g_OnMouseMove_Original), Hooked_OnMouseMove);
    Memory::SetHook(true, reinterpret_cast<void**>(&g_DestructorOnCreate_Original), Hooked_DestructorOnCreate);
    Memory::SetHook(true, reinterpret_cast<void**>(&g_Draw_Original), Hooked_Draw);
    Memory::SetHook(true, reinterpret_cast<void**>(&g_OnPacket_Original), Hooked_OnPacket_UserInfoEx);
}
