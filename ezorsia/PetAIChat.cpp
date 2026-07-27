#include "stdafx.h"
#include "PetAIChat.h"
#include "PetHelper_Internal.h"
#include "UnicodeHook.h"
#include <string>
#include <thread>
#include <mutex>
#include <iostream>
#include <sstream>
#include <windows.h>
#include <wininet.h>
#include <comutil.h>
#include <mmsystem.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "comsuppw.lib")
#pragma comment(lib, "winmm.lib")

namespace PetAIChat {

    static std::string g_apiKey = "";
    static bool g_enabled = true;
    static std::string g_petName = "小白";

    // The Gemini HTTP call runs on a detached background thread (see
    // OnPlayerChat below). CPet::DoAction touches animation/UI state
    // (CAnimationDisplayer::LoadLayer, CChatBalloon::MakeBalloon) that the
    // client's own game loop assumes is only ever touched from the main
    // thread. Calling it directly from the HTTP thread is a race against the
    // main thread's own per-frame pet update - MakePetSay must only run on
    // the main thread. The background thread stores its result here instead
    // of calling MakePetSay directly; PumpPendingSpeech() (called from the
    // main-thread pet-update hook in PetHelper.cpp) drains it.
    static std::mutex g_pendingSpeechMutex;
    static std::string g_pendingSpeechText;
    static bool g_pendingSpeechFlag = false;

    // Typedef for CPet::DoAction (__thiscall)
    // void* __thiscall CPet::DoAction(void* pCPet, int a2, int a3, void* pBstrData, int a5, int a6)
    //
    // pBstrData is a _bstr_t passed BY VALUE (confirmed via decompilation of
    // CPet::ChatCommand, the client's own native caller of this function):
    // it's a single 4-byte _bstr_t::Data_t* placed directly in the argument
    // slot, not a pointer to a _bstr_t object. DoAction reads *pBstrData as
    // the raw BSTR (length prefix at *pBstrData - 4), AddRefs a copy for the
    // chat balloon, and at the end calls _bstr_t::Data_t::Release(pBstrData)
    // to release the one reference it was handed (standard "callee destroys
    // its by-value parameter" C++ ABI). Data_t is allocated/freed via the
    // client's own ZAllocEx<ZAllocAnonSelector> allocator (see 0x402ea5 /
    // 0x406301), NOT the CRT heap - so it must be constructed via the
    // client's own _bstr_t(const char*) constructor below, never via our
    // own _bstr_t/SysAllocString (that would hand Release() a pointer our
    // DLL's CRT allocated, which it then frees with the wrong allocator ->
    // heap corruption -> the disconnect-after-pet-speaks bug).
    typedef void* (__thiscall* CPet__DoAction_t)(void* pThis, int a2, int a3, void* pBstrData, int a5, int a6);
    static CPet__DoAction_t CPet__DoAction = reinterpret_cast<CPet__DoAction_t>(0x007055E2);

    // Client's own _bstr_t::_bstr_t(const char*) constructor (ctor, not ours).
    // Writes the resulting Data_t* into *pOutSlot. The char* is converted via
    // the client's internal MultiByteToWideChar call, which UnicodeHook has
    // globally forced to CP_UTF8, so a UTF-8 `str` here decodes correctly.
    typedef void(__thiscall* Client_bstr_t_ctor_t)(void* pOutSlot, const char* str);
    static Client_bstr_t_ctor_t Client_bstr_t_ctor = reinterpret_cast<Client_bstr_t_ctor_t>(0x00406301);

    // ---------------------------------------------------------------------------
    //  WinINet Helper: Send HTTPS POST to Google Gemini API (gemini-2.5-flash)
    // ---------------------------------------------------------------------------
    static std::string HttpPostGemini(const std::string& apiKey, const std::string& prompt) {
        HINTERNET hInternet = InternetOpenA("MaplePetAI/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
        if (!hInternet) {
            std::cout << "[PetAI] InternetOpen failed: " << GetLastError() << "\n";
            return "";
        }

        // Set connect/receive timeout (10s)
        DWORD timeout = 10000;
        InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        HINTERNET hConnect = InternetConnectA(hInternet, "generativelanguage.googleapis.com",
            INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        if (!hConnect) {
            std::cout << "[PetAI] InternetConnect failed: " << GetLastError() << "\n";
            InternetCloseHandle(hInternet);
            return "";
        }

        std::string urlPath = "/v1beta/models/gemini-3.1-flash-lite:generateContent?key=" + apiKey;
        HINTERNET hRequest = HttpOpenRequestA(hConnect, "POST", urlPath.c_str(), NULL, NULL, NULL,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!hRequest) {
            std::cout << "[PetAI] HttpOpenRequest failed: " << GetLastError() << "\n";
            InternetCloseHandle(hConnect);
            InternetCloseHandle(hInternet);
            return "";
        }

        // Bypass SSL certificate errors (self-signed / corp proxy)
        DWORD dwFlags = 0;
        DWORD dwSize = sizeof(dwFlags);
        InternetQueryOptionA(hRequest, INTERNET_OPTION_SECURITY_FLAGS, &dwFlags, &dwSize);
        dwFlags |= SECURITY_FLAG_IGNORE_UNKNOWN_CA
                 | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                 | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        InternetSetOptionA(hRequest, INTERNET_OPTION_SECURITY_FLAGS, &dwFlags, sizeof(dwFlags));

        // Construct JSON payload
        std::string systemPrompt = "You are a cute, caring pet in MapleStory. Reply in short Traditional Chinese (under 20 words).";

        // Escape the user prompt for safe JSON embedding
        std::string escapedPrompt;
        for (unsigned char c : prompt) {
            if (c == '"')       escapedPrompt += "\\\"";
            else if (c == '\\') escapedPrompt += "\\\\";
            else if (c == '\n') escapedPrompt += "\\n";
            else if (c == '\r') escapedPrompt += "\\r";
            else                escapedPrompt += (char)c;
        }

        std::ostringstream jsonStream;
        jsonStream << "{\n"
            << "  \"system_instruction\": {\n"
            << "    \"parts\": [ { \"text\": \"" << systemPrompt << "\" } ]\n"
            << "  },\n"
            << "  \"contents\": [\n"
            << "    { \"parts\": [ { \"text\": \"" << escapedPrompt << "\" } ] }\n"
            << "  ]\n"
            << "}";

        std::string payload = jsonStream.str();
        std::string headers = "Content-Type: application/json\r\n";

        std::cout << "[PetAI] Sending POST, payload size=" << payload.size() << "\n";

        BOOL bSend = HttpSendRequestA(hRequest, headers.c_str(), (DWORD)headers.length(),
            (LPVOID)payload.c_str(), (DWORD)payload.length());

        std::string responseStr = "";
        if (bSend) {
            // Read HTTP status code
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            HttpQueryInfoA(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                           &statusCode, &statusSize, NULL);
            std::cout << "[PetAI] HTTP status: " << statusCode << "\n";

            char buffer[4096];
            DWORD bytesRead = 0;
            while (InternetReadFile(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                responseStr += buffer;
            }
            std::cout << "[PetAI] Response length: " << responseStr.size() << "\n";
            if (responseStr.size() < 1000)
                std::cout << "[PetAI] Response: " << responseStr << "\n";
            else
                std::cout << "[PetAI] Response (first 500): " << responseStr.substr(0, 500) << "\n";
        } else {
            std::cout << "[PetAI] HttpSendRequest failed: " << GetLastError() << "\n";
        }

        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return responseStr;
    }

    // ---------------------------------------------------------------------------
    //  Extract text field from Gemini JSON response
    // ---------------------------------------------------------------------------
    static std::string ParseGeminiText(const std::string& json) {
        size_t textPos = json.find("\"text\": \"");
        if (textPos == std::string::npos) return "";

        textPos += 9; // Skip `"text": "`
        size_t endPos = json.find("\"", textPos);
        if (endPos == std::string::npos) return "";

        std::string rawText = json.substr(textPos, endPos - textPos);
        std::string cleanText = "";
        for (size_t i = 0; i < rawText.length(); i++) {
            if (rawText[i] == '\\' && i + 1 < rawText.length()) {
                if (rawText[i + 1] == 'n') { cleanText += ' '; i++; continue; }
                if (rawText[i + 1] == '"') { cleanText += '"'; i++; continue; }
            }
            cleanText += rawText[i];
        }
        return cleanText;
    }

    // ---------------------------------------------------------------------------
    //  Public Interface
    // ---------------------------------------------------------------------------
    void Init() {
        char buf[256] = { 0 };
        GetPrivateProfileStringA("PetAI", "GeminiApiKey", "", buf, sizeof(buf), ".\\config.ini");
        if (strlen(buf) > 0) {
            g_apiKey = buf;
            std::cout << "[PetAI Chat] API Key loaded from config.ini\n";
        }
    }

    void SetApiKey(const std::string& key) {
        g_apiKey = key;
    }

    // Convert ANSI/Big5 to UTF-8
    // Uses UnicodeHook's Real_* passthrough: once UnicodeHook::ApplyHooks() is
    // active, the raw WinAPI symbols are Detours-patched process-wide to force
    // CP_UTF8, so a direct MultiByteToWideChar(CP_ACP, ...) call here would be
    // silently coerced to CP_UTF8 and corrupt Big5 chat input.
    static std::string AnsiToUtf8(const std::string& ansiStr) {
        if (ansiStr.empty()) return "";
        int wsize = UnicodeHook::RealMultiByteToWideChar(CP_ACP, 0, ansiStr.c_str(), (int)ansiStr.size(), NULL, 0);
        std::wstring wstr(wsize, 0);
        UnicodeHook::RealMultiByteToWideChar(CP_ACP, 0, ansiStr.c_str(), (int)ansiStr.size(), &wstr[0], wsize);

        int u8size = UnicodeHook::RealWideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string u8str(u8size, 0);
        UnicodeHook::RealWideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &u8str[0], u8size, NULL, NULL);
        return u8str;
    }

    void MakePetSay(int petIdx, const std::string& text) {
        void* pCPet = nullptr;
        for (int i = 0; i < 3; i++) {
            if (PetHelper::g_pets[i].pCPet) {
                pCPet = PetHelper::g_pets[i].pCPet;
                petIdx = i;
                break;
            }
        }

        if (!pCPet && PetHelper::g_lastActivePet) {
            pCPet = PetHelper::g_lastActivePet;
            petIdx = 0;
            std::cout << "[PetAI Chat] g_pets empty, using fallback g_lastActivePet: " << pCPet << "\n";
        }

        if (!pCPet) {
            std::cout << "[PetAI Chat] Error: No active pet found in memory (g_pets is empty).\n";
            return;
        }

        std::cout << "[PetAI Chat] Pet slot " << petIdx << " speaking: " << text << "\n";

        // Build the _bstr_t Data_t* using the CLIENT's own constructor (not
        // our CRT's _bstr_t/SysAllocString) so it's allocated with the same
        // allocator DoAction will eventually release it with. See the long
        // comment on CPet__DoAction_t above for why this matters.
        void* bstrSlot = nullptr;
        Client_bstr_t_ctor(&bstrSlot, text.c_str());
        if (bstrSlot) {
            // DoAction takes ownership of this one reference and releases it
            // internally - don't release bstrSlot ourselves after this call.
            CPet__DoAction(pCPet, 2, 0, bstrSlot, 0, timeGetTime());
            std::cout << "[PetAI Chat] CPet::DoAction called successfully!\n";
        }
    }

    // Call from the main thread only (see PetHelper.cpp's per-frame pet
    // update hook, Hooked_WorkUpdateActive). Drains a reply queued by the
    // background Gemini thread, if any, and makes the pet say it.
    void PumpPendingSpeech() {
        std::string text;
        {
            std::lock_guard<std::mutex> lock(g_pendingSpeechMutex);
            if (!g_pendingSpeechFlag) return;
            text = g_pendingSpeechText;
            g_pendingSpeechFlag = false;
        }
        MakePetSay(0, text);
    }

    void OnPlayerChat(const std::string& message) {
        if (!g_enabled || message.empty()) return;

        // message is already UTF-8 here: it's read straight off the outgoing
        // chat packet, and UnicodeHook forces WideCharToMultiByte to CP_UTF8
        // process-wide, so the client's own wide->narrow packet build already
        // produced UTF-8 bytes. Running AnsiToUtf8 on it again double-converts
        // and corrupts it (or throws on malformed DBCS decode), so don't.
        std::string prompt = message;
        bool triggered = false;

        // Trigger on ": <message>"
        if (prompt.rfind(":", 0) == 0) {
            prompt = prompt.substr(1);
            size_t start = prompt.find_first_not_of(" \t");
            if (start != std::string::npos) prompt = prompt.substr(start);
            triggered = true;
        }

        if (!triggered) return;

        std::cout << "[PetAI Chat] Triggered prompt (UTF-8): " << prompt << "\n";

        std::thread([prompt]() {
            if (g_apiKey.empty()) {
                std::cout << "[PetAI Chat] Please set GeminiApiKey in config.ini!\n";
                return;
            }

            std::cout << "[PetAI Chat] Requesting Gemini API...\n";
            std::string jsonResp = HttpPostGemini(g_apiKey, prompt);
            std::string aiReply = ParseGeminiText(jsonResp);

            if (!aiReply.empty()) {
                std::cout << "[PetAI Chat] AI Replied: " << aiReply << "\n";
                // Don't call MakePetSay here - this is a background thread.
                // Queue it for the main thread to pick up instead.
                {
                    std::lock_guard<std::mutex> lock(g_pendingSpeechMutex);
                    g_pendingSpeechText = aiReply;
                    g_pendingSpeechFlag = true;
                }
            } else {
                std::cout << "[PetAI Chat] API request failed or empty reply.\n";
            }
        }).detach();
    }

    // ---------------------------------------------------------------------------
    //  Hook CClientSocket::SendPacket (0x0049637B)
    //  Intercept outgoing player chat packets (Opcode 0x0031 / 0x002A)
    // ---------------------------------------------------------------------------
    struct COutPacket {
        int Loopback;
        union {
            unsigned char* Data;
            void* Unk;
            unsigned short* Header;
        };
        unsigned long Size;
        unsigned int Offset;
        int EncryptedByShanda;
    };

    typedef void (__fastcall* SendPacket_t)(void* pThis, void* edx, COutPacket* pPacket);
    static SendPacket_t Original_SendPacket = reinterpret_cast<SendPacket_t>(0x0049637B);

    static bool TryExtractChatPacket(COutPacket* pPacket, std::string& outMsg) {
        __try {
            if (!pPacket || !pPacket->Data || pPacket->Size < 4) return false;
            const unsigned short opcode = *reinterpret_cast<const unsigned short*>(pPacket->Data);
            // CP_UserChat in v83 is 0x0031 (49) or 0x002A (42)
            if (opcode == 0x0031 || opcode == 0x002A) {
                // Packet layout: WORD opcode (2 bytes at Data+0) + WORD stringLen (2 bytes at Data+2) + string chars (at Data+4)
                const unsigned short stringLen = *reinterpret_cast<const unsigned short*>(pPacket->Data + 2);
                if (stringLen > 0 && stringLen < 500 && pPacket->Size >= static_cast<unsigned long>(4 + stringLen)) {
                    outMsg.assign(reinterpret_cast<const char*>(pPacket->Data + 4), stringLen);
                    return true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return false;
    }

    static void __fastcall Hooked_SendPacket(void* pThis, void* edx, COutPacket* pPacket) {
        if (pPacket && pPacket->Data && pPacket->Size >= 2) {
            unsigned short op = *reinterpret_cast<unsigned short*>(pPacket->Data);
            std::string chatMsg = "";
            if (TryExtractChatPacket(pPacket, chatMsg)) {
                std::cout << "[SendPacket] Chat Opcode: 0x" << std::hex << op << " Text: " << chatMsg << std::dec << "\n";
                OnPlayerChat(chatMsg);
            }
        }
        Original_SendPacket(pThis, edx, pPacket);
    }

    bool Hook_SendChatMsg(bool bEnable) {
        return Memory::SetHook(bEnable, reinterpret_cast<void**>(&Original_SendPacket), Hooked_SendPacket);
    }

} // namespace PetAIChat
