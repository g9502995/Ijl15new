#include "stdafx.h"
#include "PetHelper_Internal.h"
#include <string>
#include <thread>
#include <iostream>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

// ===========================================================================
//  PetAIChat - Connects Pet AI to Google Gemini / Groq API for in-game chatting
// ===========================================================================
namespace PetAIChat {

    static std::string g_apiKey = ""; // USER API Key goes here
    static bool g_enabled = true;

    // Send HTTP POST request asynchronously to Gemini API
    void RequestAIResponse(const std::string& userSpeech) {
        std::thread([userSpeech]() {
            if (g_apiKey.empty()) {
                std::cout << "[PetAI Chat] API Key not set!\n";
                return;
            }

            // HTTP POST request template for Google Gemini API
            std::cout << "[PetAI Chat] User said: " << userSpeech << "\n";
            // TODO: Hook CPet::ShowSay or Chat Bubble packet to render AI response above pet
        }).detach();
    }

    void OnPlayerChat(const std::string& message) {
        if (!g_enabled) return;
        // Example trigger: `:小白 說話` or `小白 ...`
        if (message.rfind("小白", 0) == 0 || message.rfind(":", 0) == 0) {
            RequestAIResponse(message);
        }
    }

} // namespace PetAIChat
