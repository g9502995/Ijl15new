#pragma once
#include <string>

namespace PetAIChat {
    void Init();
    void SetApiKey(const std::string& key);
    void OnPlayerChat(const std::string& message);
    void MakePetSay(int petIdx, const std::string& text);
}
