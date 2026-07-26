#include "stdafx.h"
#include "PetAIChat.h"
#include "PetHelper_Internal.h"
#include <string>
#include <thread>
#include <iostream>
#include <sstream>
#include <windows.h>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

namespace PetAIChat {

    static std::string g_apiKey = "";
    static bool g_enabled = true;
    static std::string g_petName = "Pet";

    // ---------------------------------------------------------------------------
    //  WinINet Helper: Send HTTPS POST to Google Gemini API (gemini-2.5-flash)
    // ---------------------------------------------------------------------------
    static std::string HttpPostGemini(const std::string& apiKey, const std::string& prompt) {
        HINTERNET hInternet = InternetOpenA("MaplePetAI/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!hInternet) return "";

        HINTERNET hConnect = InternetConnectA(hInternet, "generativelanguage.googleapis.com",
            INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        if (!hConnect) {
            InternetCloseHandle(hInternet);
            return "";
        }

        std::string urlPath = "/v1beta/models/gemini-2.5-flash:generateContent?key=" + apiKey;
        HINTERNET hRequest = HttpOpenRequestA(hConnect, "POST", urlPath.c_str(), NULL, NULL, NULL,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
        if (!hRequest) {
            InternetCloseHandle(hConnect);
            InternetCloseHandle(hInternet);
            return "";
        }

        // Construct JSON Payload with ASCII escaped strings
        std::string systemPrompt = "You are a cute, caring pet in MapleStory. Reply in short Traditional Chinese (under 20 words).";

        std::ostringstream jsonStream;
        jsonStream << "{\n"
            << "  \"system_instruction\": {\n"
            << "    \"parts\": [ { \"text\": \"" << systemPrompt << "\" } ]\n"
            << "  },\n"
            << "  \"contents\": [\n"
            << "    { \"parts\": [ { \"text\": \"" << prompt << "\" } ] }\n"
            << "  ]\n"
            << "}";

        std::string payload = jsonStream.str();
        std::string headers = "Content-Type: application/json\r\n";

        BOOL bSend = HttpSendRequestA(hRequest, headers.c_str(), (DWORD)headers.length(),
            (LPVOID)payload.c_str(), (DWORD)payload.length());

        std::string responseStr = "";
        if (bSend) {
            char buffer[2048];
            DWORD bytesRead = 0;
            while (InternetReadFile(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                responseStr += buffer;
            }
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

    void MakePetSay(int petIdx, const std::string& text) {
        if (petIdx < 0 || petIdx > 2) return;
        void* pCPet = PetHelper::g_pets[petIdx].pCPet;
        if (!pCPet) return;

        std::cout << "[PetAI Chat] Pet " << petIdx << " says: " << text << "\n";
        // TODO: Trigger native CPet::ShowSay
    }

    void OnPlayerChat(const std::string& message) {
        if (!g_enabled || message.empty()) return;

        std::string prompt = message;
        bool triggered = false;

        if (prompt.rfind(":", 0) == 0) {
            prompt = prompt.substr(1);
            triggered = true;
        }

        if (!triggered) return;

        std::cout << "[PetAI Chat] Triggered prompt: " << prompt << "\n";

        std::thread([prompt]() {
            if (g_apiKey.empty()) {
                std::cout << "[PetAI Chat] Please set GeminiApiKey in config.ini!\n";
                return;
            }

            std::string jsonResp = HttpPostGemini(g_apiKey, prompt);
            std::string aiReply = ParseGeminiText(jsonResp);

            if (!aiReply.empty()) {
                MakePetSay(0, aiReply);
            } else {
                std::cout << "[PetAI Chat] API request failed or empty reply.\n";
            }
        }).detach();
    }

} // namespace PetAIChat
