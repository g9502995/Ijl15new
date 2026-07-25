#include "stdafx.h"
#include "PetHelper_Internal.h"

// ===========================================================================
//  PetController - turns "this edge, this direction" into stick input for a
//  single pet, plus the route-cache glue that decides when a cached route is
//  still good enough to keep using.
// ===========================================================================
namespace PetHelper {

    void PetController::DownJump(void* pThis, int petX, int petY, int dir) {
        int action = (dir < 0) ? 5 : 4;
        *(void**)((char*)pThis + VECCTRL_FOOTHOLD) = NULL;
        g_SetActive(pThis, 1, petX, petY + 6, 0, g_config.downjumpVy, action, NULL);
    }

    // kind: 0 none, 1 normal jump, 2 down-jump
    void PetController::ApplyMove(void* pThis, int petIdx, int dir, int kind,
        int petX, int petY)
    {
        DWORD now = GetTickCount();
        bool  inAir = false;

        if (petIdx >= 0 && petIdx <= 2) {
            PetSlotContext& pet = g_pets[petIdx];
            if (now - pet.lastJumpTime < (DWORD)g_config.airLockMs) {
                inAir = true;
                if (pet.lastJumpDir != 0) dir = pet.lastJumpDir;
            }
        }

        g_SetInput(pThis, dir, 0);

        if (kind == 0 || inAir) return;
        if (petIdx >= 0 && petIdx <= 2) {
            PetSlotContext& pet = g_pets[petIdx];
            if (now - pet.lastJumpTime < (DWORD)g_config.jumpCooldownMs) return;
            pet.lastJumpTime = now;
            pet.lastJumpDir = dir;
        }

        if (kind == 2 && g_config.useDownjump) DownJump(pThis, petX, petY, dir);
        else *(int*)((char*)pThis + VECCTRL_JUMP_FLAG) = 1;
    }

    // Turn a desired X into a direction, with a dead zone and hysteresis so the
    // pet stops instead of flickering when it is already close enough.
    int PetController::SteerTo(int petIdx, int petX, int wantX)
    {
        if (petIdx < 0 || petIdx > 2) return 0;
        PetSlotContext& pet = g_pets[petIdx];

        int gap = wantX - petX;
        int dir;

        if (abs(gap) <= g_config.arriveTol) {
            dir = 0;
        }
        else if (pet.lastDir != 0 &&
            ((gap > 0) != (pet.lastDir > 0)) &&
            abs(gap) < g_config.dirHysteresis) {
            dir = 0;
        }
        else {
            dir = (gap > 0) ? 1 : -1;
        }
        pet.lastDir = dir;
        return dir;
    }

    // ===========================================================================
    //  Route cache helpers
    // ===========================================================================

    static int LocateInRoute(const PetRoute& r, void* startFh)
    {
        for (size_t i = 0; i < r.steps.size(); i++)
            if (r.steps[i].fh == startFh) return (int)i;
        return -1;
    }

    bool EnsureRoute(int petIdx, void* startFh, void* goalFh,
        int tx, int ty, DWORD now, int& outIndex)
    {
        if (petIdx < 0 || petIdx > 2) return false;
        PetRoute& r = g_pets[petIdx].route;

        bool stale = !r.valid
            || abs(r.tgtX - tx) > 10 || abs(r.tgtY - ty) > 10
            || (now - r.time) > g_config.pathCacheMs;

        if (!stale) {
            outIndex = LocateInRoute(r, startFh);
            if (outIndex >= 0) return true;      // still on the planned route
        }

        if (!Pathfinder::FindRoute(startFh, goalFh, r.steps)) {
            r.valid = false;
            return false;
        }
        r.startFh = startFh;
        r.tgtX = tx;
        r.tgtY = ty;
        r.time = now;
        r.valid = true;
        outIndex = 0;
        return true;
    }

} // namespace PetHelper

