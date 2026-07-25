#pragma once

#include <windows.h>

namespace PetHelper {
    // Initializes and applies all pet hacks/modifications.
    // Call this function once during your DLL entry point (DllMain) or initialization.
    void ApplyPatches();

    // In-game ImGui overlay showing the foothold graph + each pet's route.
    // Call once per frame from the D3D EndScene hook, inside the same
    // ImGui::NewFrame()/ImGui::Render() pair used for other overlays.
    // Self-toggles visibility on F9.
    void DrawPetHelperDebugImGui();
}
