#include "stdafx.h"
#include "CustomEquipTooltipHook.h"
#include "Memory.h"
#include <map>
#include <mutex>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

typedef void(__fastcall* AddInfoEx_t)(
    int pThis,
    void* edx,
    int type,
    int a3,
    DWORD str1,
    DWORD str2,
    int color1,
    int color2);
typedef void(__fastcall* CClientSocket__OnPacket_t)(void* pThis, void* edx, void* pPacket);
typedef int(__thiscall* CUIPetEquip_OnMouseMove_t)(void* pThis, int x, int y);
typedef int(__thiscall* CUIPetEquip_HitTest_t)(void* pThis, int x, int y);
typedef unsigned short(__thiscall* CInPacket__Decode2_t)(void* pPacket);
typedef unsigned int(__thiscall* CInPacket__Decode4_t)(void* pPacket);
typedef void(__thiscall* COutPacket_ctor_t)(void* pThis, int nOpcode);
typedef void(__thiscall* COutPacket_Encode2_t)(void* pThis, unsigned short value);
typedef void(__thiscall* SendPacket_t)(void* pSocket, void* pPacket);

static AddInfoEx_t _AddInfoEx = reinterpret_cast<AddInfoEx_t>(0x008F39E1);
static CClientSocket__OnPacket_t _CClientSocket__OnPacket = reinterpret_cast<CClientSocket__OnPacket_t>(0x004965F1);
static CUIPetEquip_OnMouseMove_t _CUIPetEquip_OnMouseMove = reinterpret_cast<CUIPetEquip_OnMouseMove_t>(0x00800F7B);
static CUIPetEquip_HitTest_t _CUIPetEquip_HitTest = reinterpret_cast<CUIPetEquip_HitTest_t>(0x008011FA);
static CInPacket__Decode2_t _CInPacket__Decode2 = reinterpret_cast<CInPacket__Decode2_t>(0x0042470C);
static CInPacket__Decode4_t _CInPacket__Decode4 = reinterpret_cast<CInPacket__Decode4_t>(0x00406629);
static COutPacket_ctor_t _COutPacket_ctor = reinterpret_cast<COutPacket_ctor_t>(0x006EC9CE);
static COutPacket_Encode2_t _COutPacket_Encode2 = reinterpret_cast<COutPacket_Encode2_t>(0x00427F74);
static SendPacket_t _SendPacket = reinterpret_cast<SendPacket_t>(0x0049637B);
static void*** g_pClientSocketPtr = reinterpret_cast<void***>(0x00BE7914);

static const unsigned short SEND_CUSTOM_EQUIP_INFO = 0x1234;
static const unsigned short RECV_CUSTOM_EQUIP_INFO = 0x5678;

#pragma pack(push, 1)
struct FakeZXStringBuf {
    int nRef;
    int nByteLen;
    int nCap;
    char data[256];
};
#pragma pack(pop)

static FakeZXStringBuf g_extraBuf = { 0x7FFFFFFF, 0, 255, "" };
static FakeZXStringBuf g_valueBuf = { 0x7FFFFFFF, 0, 255, "" };
static std::map<short, int> g_CustomEquipCounts;
static std::map<short, time_t> g_LastRequestTime;
static std::mutex g_CustomEquipMutex;
static short g_CurrentEquipRawPos = 0;
static short g_LastNormalBodyRaw = 0;

const DWORD dwHookEquipHeightTarget = 0x008E8E31;
const DWORD dwHookEquipHeightReturn = 0x008E8E38;
const DWORD dwHookEquipTailTarget = 0x008E9548;
const DWORD dwHookEquipTailReturn = 0x008E9551;
const DWORD dwHookEquipFinalizeTarget = 0x007FEA55;
const DWORD dwHookEquipFinalizeReturn = 0x007FEA5A;
const DWORD dwHookInventoryMoveCaptureTarget = 0x0081D907;
const DWORD dwHookInventoryMoveCaptureReturn = 0x0081D913;
const DWORD dwHookEquipMoveCaptureTarget = 0x007FEA04;
const DWORD dwHookEquipMoveCaptureReturn = 0x007FEA09;
const DWORD dwHookRingHeightTarget = 0x008E7AE9;
const DWORD dwHookRingHeightReturn = 0x008E7AF0;
const DWORD dwHookRingDescTarget = 0x008E7B3B;
const DWORD dwHookRingDescReturn = 0x008E7B42;
static const DWORD kEquip2FirstReturn = 0x008EBAC5;
static const DWORD kEquip2SecondReturn = 0x008EBB03;

static void DebugLogCustomEquip(const char* format, ...) {
    char buffer[512] = { 0 };
    va_list args;
    va_start(args, format);
    vsprintf_s(buffer, format, args);
    va_end(args);
    OutputDebugStringA(buffer);
}

void SetCurrentEquipRawPos(short rawPos) {
    g_CurrentEquipRawPos = rawPos;
}

short GetCurrentEquipRawPos() {
    return g_CurrentEquipRawPos;
}

void SetCustomEquipCount(short rawPos, int count) {
    std::lock_guard<std::mutex> lock(g_CustomEquipMutex);
    g_CustomEquipCounts[rawPos] = count;
}

int GetCustomEquipCount(short rawPos, bool* hasValue) {
    std::lock_guard<std::mutex> lock(g_CustomEquipMutex);
    const auto it = g_CustomEquipCounts.find(rawPos);
    if (it == g_CustomEquipCounts.end()) {
        if (hasValue) {
            *hasValue = false;
        }
        return 0;
    }

    if (hasValue) {
        *hasValue = true;
    }
    return it->second;
}

static short ResolveCustomEquipRawPos(void* pEquip) {
    UNREFERENCED_PARAMETER(pEquip);
    return GetCurrentEquipRawPos();
}

static void SendRequestCustomEquipInfo(short rawPos) {
    if (!g_pClientSocketPtr || !*g_pClientSocketPtr || rawPos == 0) {
        return;
    }

    DebugLogCustomEquip("[CustomEquip] send rawPos=%d currentRaw=%d\n", rawPos, GetCurrentEquipRawPos());

    char oPacket[0x40] = { 0 };
    _COutPacket_ctor(oPacket, SEND_CUSTOM_EQUIP_INFO);
    _COutPacket_Encode2(oPacket, static_cast<unsigned short>(rawPos));
    _SendPacket(*g_pClientSocketPtr, oPacket);
}

static int __stdcall GetCustomTooltipExtraHeight() {
    return 0;
}

static void __stdcall AppendCustomTooltipData(int pToolTip, void* pEquip) {
    if (!pToolTip) {
        return;
    }

    const short rawPos = ResolveCustomEquipRawPos(pEquip);
    DebugLogCustomEquip(
        "[CustomEquip] tooltip pEquip=%p resolvedRaw=%d currentRaw=%d\n",
        pEquip,
        rawPos,
        GetCurrentEquipRawPos());

    bool hasValue = false;
    const int customValue = GetCustomEquipCount(rawPos, &hasValue);

    if (rawPos != 0) {
        const time_t now = time(NULL);
        if (now - g_LastRequestTime[rawPos] > 1) {
            g_LastRequestTime[rawPos] = now;
            SendRequestCustomEquipInfo(rawPos);
        }
    }

    if (customValue <= 0) {
        return;
    }

    sprintf_s(g_extraBuf.data, "改造次数 : ");
    g_extraBuf.nByteLen = static_cast<int>(strlen(g_extraBuf.data));

    sprintf_s(g_valueBuf.data, "%d", customValue);
    g_valueBuf.nByteLen = static_cast<int>(strlen(g_valueBuf.data));

    _AddInfoEx(
        pToolTip,
        nullptr,
        14,
        0x10,
        reinterpret_cast<DWORD>(g_extraBuf.data),
        reinterpret_cast<DWORD>(g_valueBuf.data),
        1,
        1001);
}

static void __stdcall CaptureEquippedBodyPart(int bodyPart) {
    if (bodyPart > 0 && bodyPart < 100) {
        SetCurrentEquipRawPos(static_cast<short>(-bodyPart));
    }
    else {
        SetCurrentEquipRawPos(0);
    }
}

static void __stdcall CaptureInventorySlotPos(int inventoryType, int slotPos) {
    if (inventoryType == 1 && slotPos > 0 && slotPos < 128) {
        SetCurrentEquipRawPos(static_cast<short>(0x0100 | (slotPos & 0xFF)));
    }
    else {
        SetCurrentEquipRawPos(0);
    }
}

static void __stdcall FinalizeEquippedRawPos(int bodyPart, int normalEquip, int cashEquip) {
    DebugLogCustomEquip(
        "[CustomEquip] finalize bodyPart=%d normal=%08X cash=%08X currentRaw=%d\n",
        bodyPart,
        normalEquip,
        cashEquip,
        GetCurrentEquipRawPos());

    if (bodyPart <= 0 || bodyPart >= 100) {
        g_LastNormalBodyRaw = 0;
        SetCurrentEquipRawPos(0);
        return;
    }

    g_LastNormalBodyRaw = static_cast<short>(-bodyPart);

    if (normalEquip) {
        SetCurrentEquipRawPos(g_LastNormalBodyRaw);
        return;
    }

    if (cashEquip) {
        SetCurrentEquipRawPos(static_cast<short>(-100 - bodyPart));
        return;
    }

    SetCurrentEquipRawPos(0);
}

static void __stdcall PrepareTooltipRawPosByCaller(DWORD returnAddress) {
    if (returnAddress == kEquip2FirstReturn) {
        if (g_LastNormalBodyRaw != 0) {
            g_CurrentEquipRawPos = g_LastNormalBodyRaw;
        }
    }
    else if (returnAddress == kEquip2SecondReturn) {
        if (g_LastNormalBodyRaw != 0) {
            const short bodyPart = static_cast<short>(-g_LastNormalBodyRaw);
            if (bodyPart > 0 && bodyPart < 100) {
                g_CurrentEquipRawPos = static_cast<short>(-100 - bodyPart);
            }
        }
    }

    DebugLogCustomEquip(
        "[CustomEquip] prepare caller=%08X currentRaw=%d normalRaw=%d\n",
        returnAddress,
        GetCurrentEquipRawPos(),
        g_LastNormalBodyRaw);
}

static void __fastcall CClientSocket__OnPacket_CustomEquip_Hook(void* pThis, void* edx, void* pPacket) {
    unsigned short* pLen = reinterpret_cast<unsigned short*>(reinterpret_cast<uintptr_t>(pPacket) + 0x0C);
    unsigned int* pReadPos = reinterpret_cast<unsigned int*>(reinterpret_cast<uintptr_t>(pPacket) + 0x14);

    if (pLen && pReadPos && (*pLen >= *pReadPos + 2)) {
        const unsigned int savedPos = *pReadPos;
        const unsigned short header = _CInPacket__Decode2(pPacket);

        if (header == RECV_CUSTOM_EQUIP_INFO) {
            if (*pLen >= *pReadPos + 6) {
                const short rawPos = static_cast<short>(_CInPacket__Decode2(pPacket));
                const int count = static_cast<int>(_CInPacket__Decode4(pPacket));
                DebugLogCustomEquip("[CustomEquip] recv rawPos=%d count=%d\n", rawPos, count);
                SetCustomEquipCount(rawPos, count);
                return;
            }
        }

        *pReadPos = savedPos;
    }

    _CClientSocket__OnPacket(pThis, edx, pPacket);
}


static int __fastcall CUIPetEquip_OnMouseMove_Hook(void* pThis, void* edx, int x, int y) {
    UNREFERENCED_PARAMETER(edx);

    const int slot = _CUIPetEquip_HitTest(reinterpret_cast<unsigned char*>(pThis) - 4, x, y);
    if (slot > 0 && slot < 100) {
        g_LastNormalBodyRaw = static_cast<short>(-slot);
        SetCurrentEquipRawPos(g_LastNormalBodyRaw);
    }
    else {
        g_LastNormalBodyRaw = 0;
        SetCurrentEquipRawPos(0);
    }

    return _CUIPetEquip_OnMouseMove(pThis, x, y);
}
__declspec(naked) void Hook_EquipHeightPadding() {
    __asm {
        lea eax, [eax + ecx + 0x0A6]
        push eax
        call GetCustomTooltipExtraHeight
        mov ecx, eax
        pop eax
        add eax, ecx
        jmp[dwHookEquipHeightReturn]
    }
}

__declspec(naked) void Hook_InventoryOnMouseMoveCapture() {
    __asm {
        pushfd
        pushad
        push dword ptr[ebp - 0x10]
        push dword ptr[esi + 0x5E0]
        call CaptureInventorySlotPos
        popad
        popfd

        push dword ptr[ebp - 0x10]
        lea eax, [ebp - 0x18]
        push dword ptr[esi + 0x5E0]
        jmp[dwHookInventoryMoveCaptureReturn]
    }
}

__declspec(naked) void Hook_EquipOnMouseMoveCapture() {
    __asm {
        mov[ebp - 1Ch], eax
        pushfd
        pushad
        push eax
        call CaptureEquippedBodyPart
        popad
        popfd
        mov eax, [ebp - 1Ch]
        neg eax
        jmp[dwHookEquipMoveCaptureReturn]
    }
}

__declspec(naked) void Hook_EquipOnMouseMoveFinalize() {
    __asm {
        pushfd
        pushad
        push dword ptr[ebp + 0x40]
        push dword ptr[ebp - 0x10]
        push dword ptr[ebp - 0x1C]
        call FinalizeEquippedRawPos
        popad
        popfd

        mov eax, [ebp - 0x10]
        cmp eax, edi
        jmp[dwHookEquipFinalizeReturn]
    }
}

__declspec(naked) void Hook_EquipRemodelInject() {
    __asm {
        pushad
        push dword ptr[ebp + 4]
        call PrepareTooltipRawPosByCaller
        push[ebp + 0xB4]
        push dword ptr[ebp - 0x24]
        call AppendCustomTooltipData
        popad

        mov ecx, [ebp + 0x18]
        mov esi, 0x00BE78D8
        jmp[dwHookEquipTailReturn]
    }
}

__declspec(naked) void Hook_RingHeightPadding() {
    __asm {
        lea eax, [eax + ecx + 0x0A6]
        push eax
        call GetCustomTooltipExtraHeight
        mov ecx, eax
        pop eax
        add eax, ecx
        jmp[dwHookRingHeightReturn]
    }
}

__declspec(naked) void Hook_RingRemodelInject() {
    __asm {
        pushad
        push[ebp + 0x10]
        push dword ptr[ebp - 0x14]
        call AppendCustomTooltipData
        popad

        lea ecx, [edi + 0x0C]
        mov byte ptr[ebp - 0x04], 0x0B
        jmp[dwHookRingDescReturn]
    }
}

void InitEquipToolTipHook() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    Memory::CodeCave(Hook_InventoryOnMouseMoveCapture, dwHookInventoryMoveCaptureTarget, 12);
    Memory::CodeCave(Hook_EquipOnMouseMoveCapture, dwHookEquipMoveCaptureTarget, 5);
    Memory::CodeCave(Hook_EquipOnMouseMoveFinalize, dwHookEquipFinalizeTarget, 5);
    Memory::CodeCave(Hook_EquipHeightPadding, dwHookEquipHeightTarget, 7);
    Memory::CodeCave(Hook_RingHeightPadding, dwHookRingHeightTarget, 7);
    Memory::CodeCave(Hook_EquipRemodelInject, dwHookEquipTailTarget, 9);
    Memory::CodeCave(Hook_RingRemodelInject, dwHookRingDescTarget, 7);
}

void InitCustomEquipSystem() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    Memory::SetHook(true, reinterpret_cast<void**>(&_CClientSocket__OnPacket), CClientSocket__OnPacket_CustomEquip_Hook);
    Memory::SetHook(true, reinterpret_cast<void**>(&_CUIPetEquip_OnMouseMove), CUIPetEquip_OnMouseMove_Hook);
    InitEquipToolTipHook();
}


