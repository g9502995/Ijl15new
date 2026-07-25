#include "stdafx.h"
#include "D3DHook.h"
#include "detours.h"
#include "vendor/d3d8to9/d3d8to9.hpp"

typedef IDirect3D8* (WINAPI *Direct3DCreate8_t)(UINT SDKVersion);
Direct3DCreate8_t Real_Direct3DCreate8 = nullptr;

extern "C" IDirect3D8 *WINAPI Direct3DCreate8(UINT SDKVersion);

IDirect3D8* WINAPI Hook_Direct3DCreate8(UINT SDKVersion)
{
    std::cout << "Hook_Direct3DCreate8 called!" << std::endl;
    return Direct3DCreate8(SDKVersion); 
}

void InitD3DHook()
{
    // Need to load d3d8.dll to get the address to hook, as the game imports it.
    // If the game links statically to d3d8.lib, it will call Direct3DCreate8 from d3d8.dll.
    // By detouring that function address, we redirect execution to us.
    // Use GetModuleHandle to avoid loading lock issues in DllMain
    HMODULE hD3D8 = GetModuleHandleA("d3d8.dll");
    if (!hD3D8) {
        // Fallback: If d3d8.dll isn't loaded (rare for this game), define behavior. 
        // For now, try LoadLibrary but ideally this code should run in a thread if d3d8 is missing.
        hD3D8 = LoadLibraryA("d3d8.dll"); 
    }

    if (hD3D8)
    {
        Real_Direct3DCreate8 = (Direct3DCreate8_t)GetProcAddress(hD3D8, "Direct3DCreate8");
        if (Real_Direct3DCreate8)
        {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)Real_Direct3DCreate8, Hook_Direct3DCreate8);
            DetourTransactionCommit();
        }
    }
}
