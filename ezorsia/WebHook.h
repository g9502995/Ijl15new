#pragma once
#include <windows.h>
#include <shellapi.h>
#include "detours.h"
#include <string>

// Structure of CInPacket for v83
struct v83_CInPacket {
    int m_bLoopback;    // 0x00
    int m_nState;       // 0x04
    unsigned char* m_aRecvBuff; // 0x08
    unsigned short m_uLength;   // 0x0C (這是 2 byte 的 short！)
    unsigned short m_uPadding;  // 0x0E
    int m_uUnknown;     // 0x10
    int m_uOffset;      // 0x14
};

typedef int(__thiscall* _OnOpenFullClientDownloadLink_t)(void* pThis, v83_CInPacket* pPacket);
_OnOpenFullClientDownloadLink_t _OnOpenFullClientDownloadLink = (_OnOpenFullClientDownloadLink_t)0x00A20AC0;

int __fastcall Hooked_OnOpenFullClientDownloadLink(void* pThis, void* edx, v83_CInPacket* pPacket) {
    if (pPacket && pPacket->m_aRecvBuff && pPacket->m_uLength >= pPacket->m_uOffset + 2) {
        // Read MapleStory short length
        short strLen = *(short*)(pPacket->m_aRecvBuff + pPacket->m_uOffset);
        
        if (pPacket->m_uLength >= pPacket->m_uOffset + 2 + strLen && strLen > 0) {
            std::string url((char*)(pPacket->m_aRecvBuff + pPacket->m_uOffset + 2), strLen);
            
            // [DEBUG] 彈出訊息框確認封包是否成功接收
            MessageBoxA(NULL, url.c_str(), "DEBUG: 收到封包並準備打開網頁", MB_OK);
            
            // Use WinExec with cmd to avoid ShellExecute DDE hangs on the network thread.
            std::string cmd = "cmd /c start \"\" \"" + url + "\"";
            WinExec(cmd.c_str(), SW_HIDE);
            
            // Fix offset to prevent CxxThrowException
            pPacket->m_uOffset = pPacket->m_uLength;
            return 1;
        } else {
            MessageBoxA(NULL, "封包長度不足或為 0", "DEBUG: 錯誤", MB_OK);
        }
    } else {
        MessageBoxA(NULL, "pPacket 或 m_aRecvBuff 為空", "DEBUG: 錯誤", MB_OK);
    }
    
    // Always set offset to length to prevent crash, avoid calling original function
    if (pPacket) {
        pPacket->m_uOffset = pPacket->m_uLength;
    }
    return 1;
}

void Hook_OnOpenFullClientDownloadLink(bool enable) {
    if (enable) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)_OnOpenFullClientDownloadLink, Hooked_OnOpenFullClientDownloadLink);
        DetourTransactionCommit();
    } else {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)_OnOpenFullClientDownloadLink, Hooked_OnOpenFullClientDownloadLink);
        DetourTransactionCommit();
    }
}
