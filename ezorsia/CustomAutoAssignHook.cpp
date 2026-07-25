#include "stdafx.h"
#include "CustomAutoAssignHook.h"
#include "Memory.h"
#include "Client.h"
#include <cstdint>

namespace {
    // 1. CUIStat::AutoApUp Hook (Intercept click on Auto Assign button)
    typedef int(__thiscall* CUIStat__AutoApUp_t)(void* pThis, int a2);
    CUIStat__AutoApUp_t g_CUIStatAutoApUp = reinterpret_cast<CUIStat__AutoApUp_t>(0x008C8692);

    // 2. CClientSocket::OnPacket Hook (Intercept custom packets 0x1236 and 0x1237 from server)
    typedef void(__fastcall* CClientSocket__OnPacket_t)(void* pThis, void* edx, void* pPacket);
    CClientSocket__OnPacket_t g_CClientSocketOnPacket = reinterpret_cast<CClientSocket__OnPacket_t>(0x004965F1);

    // 3. CUIStat::EnableApUpButton Hook (Force Auto Assign button to be enabled even with 0 AP)
    typedef void(__thiscall* CUIStat__EnableApUpButton_t)(void* pThis);
    CUIStat__EnableApUpButton_t g_CUIStatEnableApUpButton = reinterpret_cast<CUIStat__EnableApUpButton_t>(0x008C5845);

    // 4. OutPacket functions and pointers
    typedef void(__thiscall* COutPacket_ctor_t)(void* pThis, int nOpcode);
    static COutPacket_ctor_t _COutPacket_ctor_auto = reinterpret_cast<COutPacket_ctor_t>(0x006EC9CE);

    typedef void(__thiscall* COutPacket_Encode4_t)(void* pThis, unsigned int value);
    static COutPacket_Encode4_t _COutPacket_Encode4_auto = reinterpret_cast<COutPacket_Encode4_t>(0x004065A6);

    typedef void(__thiscall* CClientSocket__SendPacket_t)(void* pThis, void* pPacket);
    static CClientSocket__SendPacket_t _CClientSocketSendPacket = reinterpret_cast<CClientSocket__SendPacket_t>(0x0049637B);

    void** g_pClientSocket = reinterpret_cast<void**>(0x00BE7914);

    // --- OnPacket Hook Implementation ---
    void __fastcall CClientSocket__OnPacket_Hook(void* pThis, void* edx, void* pPacket) {
        if (pPacket != nullptr) {
            constexpr uintptr_t kInPacketDataOffset = 0x08;
            constexpr uintptr_t kInPacketLengthOffset = 0x0C;
            constexpr uintptr_t kInPacketReadPosOffset = 0x14;

            auto* data = *reinterpret_cast<const uint8_t**>(reinterpret_cast<uintptr_t>(pPacket) + kInPacketDataOffset);
            const auto readPos = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(pPacket) + kInPacketReadPosOffset);
            const auto totalLength = *reinterpret_cast<const uint16_t*>(reinterpret_cast<uintptr_t>(pPacket) + kInPacketLengthOffset);

            if (data != nullptr && totalLength >= readPos + 2) {
                uint16_t opcode = data[readPos] | (data[readPos + 1] << 8);

                if (opcode == 0x1236) {
                    if (totalLength >= readPos + 3) {
                        bool enable = (data[readPos + 2] != 0);
                        Client::autoAssignNpc = enable;
                    }
                    return;
                }
                else if (opcode == 0x1237) {
                    SendAutoAssignStatusToServer();
                    return;
                }
            }
        }

        g_CClientSocketOnPacket(pThis, edx, pPacket);
    }

    // --- CUIStat::AutoApUp Hook Implementation ---
    int __fastcall CUIStat__AutoApUp_Hook(void* pThis, void* edx, int a2) {
        if (Client::autoAssignNpc) {
            uint8_t packetBuf[256] = { 0 };
            _COutPacket_ctor_auto(packetBuf, 0x1235);
            _COutPacket_Encode4_auto(packetBuf, static_cast<unsigned int>(a2));

            if (*g_pClientSocket) {
                _CClientSocketSendPacket(*g_pClientSocket, packetBuf);
            }
            return 0;
        }
        return g_CUIStatAutoApUp(pThis, a2);
    }

    // --- CUIStat::EnableApUpButton Hook Implementation ---
    void __fastcall CUIStat__EnableApUpButton_Hook(void* pThis, void* edx) {
        g_CUIStatEnableApUpButton(pThis);

        char* pButtonsStart = reinterpret_cast<char*>(pThis) + 1508;
        for (int i = 0; i < 3; ++i) {
            void* pButton = *reinterpret_cast<void**>(pButtonsStart + i * 8);
            if (pButton != nullptr) {
                void* pCtrl = reinterpret_cast<char*>(pButton) + 4; // pCtrl is the subobject at offset 4 of pButton
                void** vtable = *reinterpret_cast<void***>(pCtrl);
                if (vtable != nullptr) {
                    typedef void(__thiscall* EnableWindow_t)(void* pThis, BOOL bEnable);
                    EnableWindow_t EnableWindow = reinterpret_cast<EnableWindow_t>(vtable[7]);
                    EnableWindow(pCtrl, TRUE);
                }
            }
        }
    }
}

// --- Public Hook Init ---
void InitCustomAutoAssignHook() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    Memory::SetHook(true, reinterpret_cast<void**>(&g_CUIStatAutoApUp), CUIStat__AutoApUp_Hook);
    Memory::SetHook(true, reinterpret_cast<void**>(&g_CClientSocketOnPacket), CClientSocket__OnPacket_Hook);
    Memory::SetHook(true, reinterpret_cast<void**>(&g_CUIStatEnableApUpButton), CUIStat__EnableApUpButton_Hook);
}

// --- Send Current Status to Server ---
void SendAutoAssignStatusToServer() {
    uint8_t packetBuf[256] = { 0 };
    _COutPacket_ctor_auto(packetBuf, 0x1237);
    _COutPacket_Encode4_auto(packetBuf, Client::autoAssignNpc ? 1 : 0);

    if (*g_pClientSocket) {
        _CClientSocketSendPacket(*g_pClientSocket, packetBuf);
    }
}

// --- Compatibility Stub for PacketHooks ---
bool HandleCustomAutoAssignPacket(void* pPacket) {
    return false;
}