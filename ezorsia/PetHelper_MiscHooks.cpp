#include "stdafx.h"
#include "detours.h"
#include "PetHelper_Internal.h"

// ===========================================================================
//  Small hooks that ride along with the pet AI but aren't part of pathing:
//    - widen a pet's on-screen bounding box when it owns a telescope (so the
//      engine doesn't cull it off-screen while it's out fetching something)
//    - cache the telescope ability flag per pet slot
//    - notice the client's "cannot pick up item" chat string and blacklist
//      whatever the nearest pet was standing on when it fired
// ===========================================================================
namespace PetHelper {

    static bool HasTelescopeSafe(void* pet)
    {
        if (!pet) return false;
        int idx = *(int*)((char*)pet + CPET_SLOT_INDEX);
        if (idx < 0 || idx > 2) return false;
        return g_pets[idx].hasTelescope;
    }

    void __stdcall MyUpdatePetBoundingBox(void* ebp_ptr, void* pet_ptr)
    {
        int l, r, t, b;
        if (HasTelescopeSafe(pet_ptr)) {
            l = g_config.limitPullbackLeft;  r = g_config.limitPullbackRight;
            t = g_config.limitPullbackTop;   b = g_config.limitPullbackBottom;
        }
        else { l = -300; r = 300; t = -200; b = 200; }
        *(int*)((char*)ebp_ptr - 0x68) = l;
        *(int*)((char*)ebp_ptr - 0x64) = t;
        *(int*)((char*)ebp_ptr - 0x60) = r;
        *(int*)((char*)ebp_ptr - 0x5C) = b;
    }

    void WriteHook()
    {
        DWORD hAddr = 0x009C443E, hLen = 28, old;
        if (!VirtualProtect((LPVOID)hAddr, hLen, PAGE_EXECUTE_READWRITE, &old)) return;
        std::vector<BYTE> sc;
        sc.push_back(0xFF); sc.push_back(0x75); sc.push_back(0xE4);
        sc.push_back(0x55);
        sc.push_back(0xE8);
        DWORD off = (DWORD)MyUpdatePetBoundingBox - (hAddr + 9);
        sc.push_back((BYTE)(off & 0xFF));
        sc.push_back((BYTE)((off >> 8) & 0xFF));
        sc.push_back((BYTE)((off >> 16) & 0xFF));
        sc.push_back((BYTE)((off >> 24) & 0xFF));
        while (sc.size() < hLen) sc.push_back(0x90);
        memcpy((void*)hAddr, sc.data(), hLen);
        VirtualProtect((LPVOID)hAddr, hLen, old, &old);
    }

    extern "C" {
        __declspec(naked) void Original_UpdatePetAbility_Trampoline(void* pThis)
        {
            __asm {
                mov eax, 0x00AADC28
                mov edx, 0x00705E31
                jmp edx
            }
        }
    }

    void __fastcall Hooked_CPet_UpdatePetAbility(void* pThis, void* /*edx*/)
    {
        Original_UpdatePetAbility_Trampoline(pThis);
        if (!pThis) return;

        int idx = *(int*)((char*)pThis + CPET_SLOT_INDEX);
        if (idx < 0 || idx > 2) return;

        int abil = 0;
        if (SEH_ReadPetAbility(pThis, &abil))
            g_pets[idx].hasTelescope = (abil != 0);
    }

    void WriteUpdatePetAbilityHook()
    {
        DWORD hAddr = 0x00705E2C;
        DWORD old;
        if (!VirtualProtect((LPVOID)hAddr, 5, PAGE_EXECUTE_READWRITE, &old)) return;
        std::vector<BYTE> sc;
        sc.push_back(0xE9);
        DWORD rel = (DWORD)Hooked_CPet_UpdatePetAbility - (hAddr + 5);
        sc.push_back((BYTE)(rel & 0xFF));
        sc.push_back((BYTE)((rel >> 8) & 0xFF));
        sc.push_back((BYTE)((rel >> 16) & 0xFF));
        sc.push_back((BYTE)((rel >> 24) & 0xFF));
        memcpy((void*)hAddr, sc.data(), 5);
        VirtualProtect((LPVOID)hAddr, 5, old, &old);
    }

    // ---------- StringPool GetString hook -----------------------------------------
    typedef void* (__thiscall* PFN_GetString)(void* pThis, void* result, int nIdx);
    static PFN_GetString Original_GetString = (PFN_GetString)0x00406455;

    class StringPoolHook {
    public:
        void* __thiscall Hooked_GetString(void* result, int nIdx) {
            if (nIdx == 295) {           // "cannot pick up item"
                DWORD now = GetTickCount();
                int bestPet = -1;
                int minOkDist = 999999;

                for (int p = 0; p < 3; p++) {
                    int tx = g_pets[p].onTopTargetX;
                    int ty = g_pets[p].onTopTargetY;
                    void* pCPet = g_pets[p].pCPet;
                    if (tx == -1 || !pCPet) continue;

                    int petX = 0, petY = 0;
                    if (!SEH_GetObjPos(pCPet, &petX, &petY)) continue;

                    int dist = abs(petX - tx) + abs(petY - ty);
                    if (dist <= 120 && dist < minOkDist) {
                        minOkDist = dist;
                        bestPet = p;
                    }
                }

                if (bestPet != -1) {
                    PetSlotContext& pet = g_pets[bestPet];
                    int tx = pet.onTopTargetX;
                    int ty = pet.onTopTargetY;

                    TargetManager::BlacklistDrop(tx, ty, now);
                    TargetManager::ClearTarget(bestPet);
                    pet.onTopTargetX = -1;
                    pet.onTopTargetY = -1;
                    pet.route.valid = false;
                    pet.petPauseUntil = now + 2000;
                }
            }
            return Original_GetString(this, result, nIdx);
        }
    };

    void WriteStringPoolHook()
    {
        union {
            void* pRaw;
            void* (StringPoolHook::* pMember)(void*, int);
        } castUnion;
        castUnion.pMember = &StringPoolHook::Hooked_GetString;
        void* pHookAddress = castUnion.pRaw;

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)Original_GetString, pHookAddress);
        DetourTransactionCommit();
        std::cout << "[PetAI] StringPool GetString hook written.\n";
    }

} // namespace PetHelper

