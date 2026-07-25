#include "stdafx.h"
#include "PetHelper.h"
#include "PetHelper_Internal.h"

// ===========================================================================
//  PetHelper.cpp - per-frame AI orchestration + hook install.
//
//  The feature used to live entirely in this one file; it is now split by
//  responsibility (see PetHelper_Internal.h for the map). This file keeps
//  only what has to run once per frame per pet (MyPetAI_Update_Inner and the
//  WorkUpdateActive/SetActive/CField::Init hooks around it) plus the
//  top-level ApplyPatches() entry point.
// ===========================================================================

// ===========================================================================
//  Naked trampolines
// ===========================================================================
extern "C" {
    __declspec(naked) int __fastcall Original_WorkUpdateActive_Trampoline(
        void* pThis, void* edxDummy, int tElapse)
    {
        __asm {
            mov     eax, 0x00AE58E8
            mov     edx, 0x009C4127
            jmp     edx
        }
    }
}
// ===========================================================================
namespace PetHelper {
    // ===========================================================================

    // Shared mutable state storage
    PetHelperConfig g_config;
    bool            g_forceMapRebuild = false;
    volatile bool   g_isMapTransitioning = false;
    PetSlotContext  g_pets[3];

    // Dummy array proxies for header backward-compatibility references
    static bool s_cachedHasTelescopeDummy[3] = { false, false, false };
    static PetRoute s_routeDummy[3];
    static int s_lastDirDummy[3] = { 0, 0, 0 };

    bool (&cachedHasTelescope)[3] = s_cachedHasTelescopeDummy;
    PetRoute (&g_route)[3] = s_routeDummy;
    int (&g_lastDir)[3] = s_lastDirDummy;

    static int g_curPetIdx = -1;

    static PFN_SetActive Original_VecCtrlPet_SetActive = NULL;
    static bool g_insideLocalAI = false;

    struct LocalAIScope {
        LocalAIScope() { g_insideLocalAI = true; }
        ~LocalAIScope() { g_insideLocalAI = false; }
    };

    void __fastcall Hooked_VecCtrl_SetActive(void* pThis, void* edxDummy, int active, int x, int y, int vx, int vy, int action, void* fh);

    // ===========================================================================
    //  Pet AI
    // ===========================================================================
    static bool MyPetAI_Update_Inner(void* pThis)
    {
        if (g_isMapTransitioning) return false;

        static DWORD lastTrace = 0;
        DWORD nowT = GetTickCount();
        bool  logNow = g_config.debugPathfind && (nowT - lastTrace >= g_config.debugIntervalMs);
        if (logNow) lastTrace = nowT;

        if (logNow) std::cout << "[PetAI] Update_Inner, pThis=" << pThis << "\n";

        if (!pThis) return false;

        DWORD rawRef = *(DWORD*)((char*)pThis + 0x14);
        if (logNow) std::cout << "[PetAI] rawRef=" << (void*)rawRef << "\n";
        if (!rawRef) return false;
        LocalAIScope scope;
        void* pCPet = (void*)(rawRef - 4);

        int petIdx = -1;
        if (!SEH_ReadInt((char*)pCPet + CPET_SLOT_INDEX, &petIdx) ||
            petIdx < 0 || petIdx > 2) {
            return false;
        }

        PetSlotContext& pet = g_pets[petIdx];

        if (pet.pCPet != pCPet) {
            pet.pCPet = pCPet;
            pet.pVecCtrl = pThis;
            pet.slotPrimed = false;
        }

        if (logNow) std::cout << "[PetAI] petIdx=" << petIdx << "\n";

        g_curPetIdx = petIdx;

        int petX = 0, petY = 0;
        bool gotPos = SEH_GetObjPos(pCPet, &petX, &petY);
        if (logNow) std::cout << "[PetAI] gotPos=" << gotPos << " pos=(" << petX << "," << petY << ")\n";

        // Paused (e.g. complaining about a full inventory): stand still.
        if (nowT < pet.petPauseUntil) {
            if (gotPos) PetController::ApplyMove(pThis, petIdx, 0, 0, petX, petY);
            return true;
        }

        if (g_config.requireTelescope && !pet.hasTelescope) {
            if (logNow) std::cout << "[T] no telescope, petIdx=" << petIdx << "\n";
            return false;
        }

        if (!gotPos) {
            if (logNow) std::cout << "[T] GetPetPos FAILED\n";
            return false;
        }
        // ---- Active rope climbing state step ------------------------------------
        if (pet.isClimbingRope) {
            int distToGoal = abs(petY - pet.ropeTargetY);
            if (distToGoal <= 12) {
                // Reached destination foothold on rope: complete climb
                g_SetActive(pThis, 1, petX, pet.ropeTargetY - 5, 0, 0, 0, pet.ropeTargetFh);
                pet.isClimbingRope = false;
                pet.ropeTargetFh = NULL;
                pet.lastMoveT = nowT;
                pet.route.valid = false;
                if (logNow) std::cout << "[T] rope climb finished -> y=" << pet.ropeTargetY << "\n";
                return true;
            } else {
                // Smoothly step vertical position along rope
                int dy = (pet.ropeTargetY > petY) ? 8 : -8;
                int nextY = petY + dy;
                if ((dy < 0 && nextY < pet.ropeTargetY) || (dy > 0 && nextY > pet.ropeTargetY)) {
                    nextY = pet.ropeTargetY;
                }
                g_SetActive(pThis, 1, petX, nextY, 0, 0, 0, NULL);
                pet.lastMoveT = nowT;
                if (logNow) std::cout << "[T] rope climbing y=" << nextY << " targetY=" << pet.ropeTargetY << "\n";
                return true;
            }
        }

        void* pUser = *(void**)((char*)pCPet + CPET_OWNER);
        void* userVec = NULL;
        void* userFh = NULL;
        if (pUser) {
            void* pComPtr = NULL;
            bool readUserVec = SEH_ReadPtr((char*)pUser + 0x11A4, &pComPtr);
            if (readUserVec && pComPtr) {
                userVec = (void*)((char*)pComPtr - 12);
            }
            if (logNow) std::cout << "[PetAI] pUser=" << pUser << " readUserVec=" << readUserVec << " userVec=" << userVec << "\n";
            if (!userVec)
                return true;
            SEH_ReadPtr((char*)userVec + VECCTRL_FOOTHOLD, &userFh);
        } else {
            if (logNow) std::cout << "[PetAI] pUser is NULL!\n";
        }

        // ---- Current foothold: read this FIRST so we can use it in all checks below --
        void* curFh = NULL;
        SEH_ReadPtr((char*)pThis + VECCTRL_FOOTHOLD, &curFh);
        if (logNow) std::cout << "[PetAI] curFh=" << curFh << "\n";

        // ---- Out of bounds fail-safe --------------------------------------------
        // Skip entirely if the pet is already standing on a valid foothold - that
        // means it has NOT fallen out of the map; the fh cache may simply be stale
        // right after a map transition and report a misleading maxFhY.
        if (gotPos && !curFh && !Pathfinder::fh.empty()) {
            int maxFhY = -999999;
            for (size_t i = 0; i < Pathfinder::fh.size(); i++) {
                if (Pathfinder::fh[i].y1 > maxFhY) maxFhY = Pathfinder::fh[i].y1;
                if (Pathfinder::fh[i].y2 > maxFhY) maxFhY = Pathfinder::fh[i].y2;
            }
            if (petY > maxFhY + 300) {
                int userX = petX, userY = petY;
                SEH_GetObjPos(pUser, &userX, &userY);
                void* targetFh = userFh;
                if (!SEH_IsValidFoothold(targetFh)) {
                    targetFh = SEH_GetFootholdUnder(userX, userY - 5, NULL);
                    if (!targetFh) targetFh = SEH_GetFootholdClosest(userX, userY);
                }
                if (SEH_IsValidFoothold(targetFh)) {
                    g_SetActive(pThis, 1, userX, userY - 5, 0, 0, 0, targetFh);
                    pet.lastMoveT = nowT;
                    TargetManager::ClearTarget(petIdx);
                    pet.route.valid = false;
                    if (logNow) std::cout << "[T] teleport to owner (out of bounds)\n";
                    return true;
                }
            }
        }

        if (!curFh) {
            PetController::ApplyMove(pThis, petIdx, pet.lastDir, 0, petX, petY);
            return true;
        }


        Pathfinder::RefreshFhCache(nowT);

        // ---- resolve an in-flight jump/down-jump now that we're grounded --------
        if (pet.pendingEdgeToIdx != -1) {
            int landedIdx = Pathfinder::FindFhIndex(curFh);
            int fromIdx = Pathfinder::FindFhIndex(pet.pendingEdgeFromFh);
            if (landedIdx == pet.pendingEdgeToIdx) {
                pet.edgeFailStreak = 0;
                if (fromIdx >= 0) Pathfinder::RecordEdgeOutcome(fromIdx, pet.pendingEdgeToIdx, true);
            }
            else if (curFh == pet.pendingEdgeFromFh) {
                // Bounced right back to where we took off: a clean failed attempt.
                pet.edgeFailStreak++;
                if (fromIdx >= 0) Pathfinder::RecordEdgeOutcome(fromIdx, pet.pendingEdgeToIdx, false);
                if (pet.edgeFailStreak >= g_config.edgeFailThreshold) {
                    if (pet.pendingEdgeToIdx >= 0 && pet.pendingEdgeToIdx < (int)Pathfinder::fh.size()) {
                        Pathfinder::BlacklistLink(pet.pendingEdgeFromFh,
                            Pathfinder::fh[pet.pendingEdgeToIdx].fh, nowT);
                    }
                    pet.route.valid = false;
                    pet.edgeFailStreak = 0;
                    if (pet.targetX != -1) {
                        TargetManager::BlacklistDrop(pet.targetX, pet.targetY, nowT);
                        TargetManager::ClearTarget(petIdx);
                    }
                    if (logNow) std::cout << "[T] edge failed " << g_config.edgeFailThreshold << "x in a row, banned\n";
                }
            }
            // Landed somewhere else entirely (drifted onto a third foothold):
            // not the intended edge, but not a clean bounce either - the
            // route will just be recomputed from wherever we ended up.
            pet.pendingEdgeToIdx = -1;
        }

        TargetManager::currentPetIdx = petIdx;
        TargetManager::ExpireLock(petIdx, nowT);
        std::vector<TargetDrop> drops =
            TargetManager::CollectSortedDrops(petX, petY, logNow, nowT);

        void* startFh = curFh;
        if (!startFh) startFh = SEH_GetFootholdUnder(petX, petY - 5, NULL);
        if (!startFh) startFh = SEH_GetFootholdClosest(petX, petY);

        bool onRoute = pet.route.valid && pet.route.steps.size() > 1;
        bool followingUser = false;
        int  tx = 0, ty = 0;

        if (pet.targetX != -1) {
            tx = pet.targetX;
            ty = pet.targetY;
            for (size_t i = 0; i < drops.size(); i++) {
                if (abs(drops[i].x - tx) <= 10 && abs(drops[i].y - ty) <= 10) {
                    tx = drops[i].x;
                    ty = drops[i].y;
                    break;
                }
            }
            pet.noDropSince = 0;
        }
        else if (!drops.empty()) {
            int pick = 0;
            if ((int)drops.size() > 1 && startFh) {
                int bestCost = INT_MAX;
                int bestPick = 0;
                int considered = (int)drops.size();
                if (considered > g_config.targetCostCandidates) considered = g_config.targetCostCandidates;
                for (int i = 0; i < considered; i++) {
                    void* dGoalFh = SEH_GetFootholdUnder(drops[i].x, drops[i].y - 5, NULL);
                    if (!dGoalFh) dGoalFh = SEH_GetFootholdClosest(drops[i].x, drops[i].y);

                    int cost;
                    if (!dGoalFh) {
                        cost = INT_MAX;
                    } else if (dGoalFh == startFh) {
                        cost = 0;
                    } else {
                        std::vector<RouteStep> tmpSteps;
                        cost = Pathfinder::FindRoute(startFh, dGoalFh, tmpSteps)
                            ? Pathfinder::RouteCost(tmpSteps) : INT_MAX;
                    }
                    if (cost < bestCost) { bestCost = cost; bestPick = i; }
                }
                pick = bestPick;
            }
            tx = drops[pick].x;
            ty = drops[pick].y;
            pet.noDropSince = 0;
        }
        else {
            if (pet.noDropSince == 0) pet.noDropSince = nowT;
            if (nowT - pet.noDropSince < g_config.noDropGraceMs) {
                PetController::ApplyMove(pThis, petIdx, pet.lastDir, 0, petX, petY);
                return true;
            }
            if (!pUser) return false;
            int userX = petX, userY = petY;
            if (!SEH_GetObjPos(pUser, &userX, &userY)) return false;
            tx = userX; ty = userY;
            followingUser = true;
        }

        // Too far from the owner: teleport onto the owner's foothold.
        if (followingUser && SEH_IsValidFoothold(userFh)) {
            if (abs(petX - tx) + abs(petY - ty) > 800) {
                g_SetActive(pThis, 1, tx, ty - 5, 0, 0, 0, userFh);
                pet.lastMoveT = nowT;
                pet.route.valid = false;
                pet.noDropSince = 0;
                if (logNow) std::cout << "[T] teleport to owner\n";
                return true;
            }
        }

        if (!followingUser) {
            if (abs(tx - pet.lastTgtX) > 10 || abs(ty - pet.lastTgtY) > 10) {
                pet.lastTgtX = tx;
                pet.lastTgtY = ty;
                pet.lastMoveT = nowT;
                pet.bestDist = abs(petX - tx) + abs(petY - ty);
                pet.route.valid = false;
            }
            else {
                // Only a real drop in distance counts as progress. Landing on
                // a *different* foothold used to reset this timer on its own,
                // which let a pet bounce between two platforms forever (every
                // landing "looked like" progress even when it wasn't) and
                // stuckMs/stuckGiveupMs below would never fire.
                int d = abs(petX - tx) + abs(petY - ty);
                if (d < pet.bestDist - 5) {
                    pet.bestDist = d;
                    pet.lastMoveT = nowT;
                }
            }
        }

        DWORD stuckFor = nowT - pet.lastMoveT;
        bool  forceJump = (stuckFor >= g_config.stuckMs);
        bool  giveUp = (stuckFor >= g_config.stuckGiveupMs);
        bool  hardGiveUp = (stuckFor >= g_config.routeGiveupMs);

        if (!followingUser && ((giveUp && !onRoute) || hardGiveUp)) {
            TargetManager::BlacklistDrop(tx, ty, nowT);
            TargetManager::ClearTarget(petIdx);
            pet.lastMoveT = nowT;
            pet.route.valid = false;
            if (logNow) std::cout << "[T] gave up on drop\n";
            return false;
        }

        if (!startFh) return false;

        void* goalFh = SEH_GetFootholdUnder(tx, ty - 5, NULL);
        if (!goalFh) goalFh = SEH_GetFootholdClosest(tx, ty);
        if (!goalFh) return false;
        if (!followingUser && goalFh == startFh) {
            // Returns false when item is confirmed unpickable (blacklisted + route cleared).
            // No teleport needed – the pet just picks a new target on the next frame.
            if (!TargetManager::CheckUnpickable(petIdx, petX, petY, tx, ty, nowT, logNow))
                return false;
        }

        else {
            pet.onTopTargetX = -1;
            pet.onTopTargetY = -1;
        }
        if (!followingUser) TargetManager::LockTarget(petIdx, tx, ty, nowT);

        // ---- already on the goal platform ---------------------------------------
        if (goalFh == startFh) {
            int wantX = tx;

            if (followingUser) {
                if (abs(tx - petX) <= 60) {
                    pet.lastMoveT = nowT;
                    pet.lastDir = 0;
                    pet.noDropSince = 0;
                    return false;
                }
                wantX = tx + (petIdx == 1 ? -30 : (petIdx == 2 ? 30 : 0));
            }

            int dir = PetController::SteerTo(petIdx, petX, wantX);
            if (dir == 0) { pet.lastMoveT = nowT; forceJump = false; }
            pet.wantGap = abs(wantX - petX);
            PetController::ApplyMove(pThis, petIdx, dir, forceJump ? 1 : 0, petX, petY);
            return true;
        }

        // ---- follow the cached route --------------------------------------------
        int idx = -1;
        if (!EnsureRoute(petIdx, startFh, goalFh, tx, ty, nowT, idx)) {
            if (!followingUser) {
                TargetManager::BlacklistDrop(tx, ty, nowT);
                TargetManager::ClearTarget(petIdx);
            }
            else if (SEH_IsValidFoothold(userFh)) {
                int userX = petX, userY = petY;
                SEH_GetObjPos(pUser, &userX, &userY);
                g_SetActive(pThis, 1, userX, userY - 5, 0, 0, 0, userFh);
                pet.lastMoveT = nowT;
                pet.noDropSince = 0;
                if (logNow) std::cout << "[T] teleport to owner (no route)\n";
                return true;
            }
            if (logNow) std::cout << "[T] no route to (" << tx << "," << ty << ")\n";
            return false;
        }

        const RouteStep& step = pet.route.steps[idx];
        if (stuckFor >= g_config.stuckGiveupMs && step.hasEdge) {
            int ti = step.edge.to;
            if (ti >= 0 && ti < (int)Pathfinder::fh.size())
                Pathfinder::BlacklistLink(step.fh, Pathfinder::fh[ti].fh, nowT);
            pet.route.valid = false;
            pet.lastMoveT = nowT;
            if (logNow) std::cout << "[T] link unusable, banned\n";
            return false;
        }
        int wantX = tx;
        int kind = 0;
        const char* modeName = "goal";

        if (step.hasEdge) {
            bool atTakeoff = (abs(petX - step.edge.takeoffX) <= g_config.actionMargin);

            if (step.edge.type == EDGE_ROPE) {
                modeName = "rope";
                wantX = step.edge.takeoffX;
                if (atTakeoff || forceJump) {
                    int ti = step.edge.to;
                    if (ti >= 0 && ti < (int)Pathfinder::fh.size()) {
                        int landY = Pathfinder::YAtX(Pathfinder::fh[ti], step.edge.landingX);
                        pet.isClimbingRope = true;
                        pet.ropeStartY = petY;
                        pet.ropeTargetY = landY;
                        pet.ropeTargetFh = Pathfinder::fh[ti].fh;
                        pet.lastMoveT = nowT;
                        if (logNow) std::cout << "[T] rope climb start -> (" << step.edge.landingX << "," << landY << ")\n";
                        return true;
                    }
                }
            }
            else if (step.edge.type == EDGE_JUMP) {
                modeName = "jump";
                if (atTakeoff || forceJump) {
                    kind = 1;
                    wantX = step.edge.landingX;
                } else {
                    wantX = step.edge.takeoffX;
                }
            }
            else if (step.edge.type == EDGE_DROP) {
                modeName = "drop";
                wantX = step.edge.takeoffX;
                if (atTakeoff || forceJump) {
                    kind = g_config.useDownjump ? 2 : 0;
                    if (kind != 2) wantX = step.edge.landingX;
                }
            }
            else {
                modeName = "walk";
                int ti = step.edge.to;
                if (ti >= 0 && ti < (int)Pathfinder::fh.size())
                    wantX = Pathfinder::MidX(Pathfinder::fh[ti]);
                else
                    wantX = step.edge.landingX;
                if (forceJump) kind = 1;
            }
        }
        else if (forceJump) {
            kind = 1;
        }

        // Arm reliability tracking for a real jump / down-jump commit (rope
        // teleports and plain walk-nudges never reach here - see above).
        if (kind != 0 && step.hasEdge &&
            (step.edge.type == EDGE_JUMP || (step.edge.type == EDGE_DROP && kind == 2))) {
            pet.pendingEdgeFromFh = step.fh;
            pet.pendingEdgeToIdx  = step.edge.to;
            pet.pendingEdgeSince  = nowT;
        }

        int dir = PetController::SteerTo(petIdx, petX, wantX);

        if (dir == 0 && kind == 0 && !step.hasEdge) pet.lastMoveT = nowT;

        if (kind != 0 && dir == 0 &&
            !(step.hasEdge && step.edge.type == EDGE_JUMP)) {
            int nudge = step.hasEdge ? step.edge.landingX : tx;
            dir = (nudge >= petX) ? 1 : -1;
            pet.lastDir = dir;
        }

        if (kind != 0 && step.hasEdge) {
            int aim = step.edge.landingX;
            if (step.edge.type == EDGE_JUMP || abs(aim - petX) > g_config.aimTol) {
                dir = (aim >= petX) ? 1 : -1;
                pet.lastDir = dir;
            }
        }

        if (logNow) {
            printf("[T] pet=(%d,%d) tgt=(%d,%d) lock=(%d,%d) age=%u step=%d/%d %s dir=%d kind=%d stuck=%u\n",
                petX, petY, tx, ty,
                pet.targetX, pet.targetY,
                nowT - pet.lockSeen,
                idx, (int)pet.route.steps.size(), modeName, dir, kind, stuckFor);
        }
        bool nearAction = step.hasEdge && step.edge.type != EDGE_WALK &&
                          abs(petX - step.edge.takeoffX) < g_config.boostBrakeGap;
        pet.wantGap = nearAction ? 0 : abs(tx - petX);
        PetController::ApplyMove(pThis, petIdx, dir, kind, petX, petY);
        return true;
    }

    // ===========================================================================
    //  Hook entry
    // ===========================================================================
    static double* GetPhysicsTable()
    {
        void* pSpace = *g_ppPhysicalSpace;
        if (!pSpace) return NULL;
        void* tbl = NULL;
        if (!SEH_ReadPtr((char*)pSpace + 8, &tbl)) return NULL;
        return (double*)tbl;
    }

    int __fastcall Hooked_WorkUpdateActive(void* pThis, void* edxDummy, int tElapse)
    {
        if (MyPetAI_Update_Inner(pThis)) {
            bool wantBoost = false;
            const int BOOST_MIN_GAP = 120;
            if (g_config.petSpeedMult > 1.0 && g_curPetIdx >= 0 && g_curPetIdx <= 2) {
                PetSlotContext& pet = g_pets[g_curPetIdx];
                int gap = pet.wantGap;
                if (!pet.boosting && gap > BOOST_MIN_GAP)
                    pet.boosting = true;
                else if (pet.boosting && gap < BOOST_MIN_GAP / 2)
                    pet.boosting = false;
                wantBoost = pet.boosting;
            }

            double* phys = wantBoost ? GetPhysicsTable() : NULL;
            double  savedSpeed = 0.0, savedForce = 0.0;
            bool    boosted = false;

            if (phys)
                boosted = (SEH_ScalePhysics(phys, g_config.petSpeedMult,
                    &savedSpeed, &savedForce) != FALSE);

            g_BaseWorkUpdateActive(pThis, tElapse);

            if (boosted) SEH_RestorePhysics(phys, savedSpeed, savedForce);
            return 1;
        }

        for (int i = 0; i < 3; i++) {
            if (g_pets[i].pVecCtrl == pThis) {
                g_pets[i].Reset();
            }
        }

        return Original_WorkUpdateActive_Trampoline(pThis, edxDummy, tElapse);
    }

    __declspec(naked) void __fastcall Original_CField_Init_Trampoline(void* pThis, void* edxDummy, void* arg0)
    {
        __asm {
            mov eax, 00A8A516h
            push 0052926Eh
            ret
        }
    }

    void __fastcall Hooked_CField_Init(void* pThis, void* edxDummy, void* arg0)
    {
        std::cout << "[PetAI] Map changed (CField::Init intercepted).\n";
        g_isMapTransitioning = true;
        g_forceMapRebuild = true;
        for (int i = 0; i < 3; i++) {
            g_pets[i].Reset();
        }
        // Coordinates are per-map; a stale entry (especially a
        // DROP_BLACKLIST_FOREVER one) must not leak into the next map and
        // misjudge an unrelated item that happens to share the same (x,y).
        TargetManager::dropBlacklist.clear();
        Original_CField_Init_Trampoline(pThis, edxDummy, arg0);
        g_isMapTransitioning = false;
    }

    // ---------- SetActive vtable hook implementation ------------------------------

    void __fastcall Hooked_VecCtrl_SetActive(void* pThis, void* edxDummy, int active, int x, int y, int vx, int vy, int action, void* fh)
    {
        if (!g_insideLocalAI && Original_VecCtrlPet_SetActive) {
            int petIdx = -1;
            for (int i = 0; i < 3; i++) {
                if (g_pets[i].pVecCtrl == pThis) {
                    petIdx = i;
                    break;
                }
            }
            if (petIdx != -1 && g_pets[petIdx].slotPrimed) {
                void* pCPet = g_pets[petIdx].pCPet;
                void* pUser = NULL;
                if (pCPet) SEH_ReadPtr((char*)pCPet + CPET_OWNER, &pUser);
                if (pUser) {
                    void* petFh = NULL;
                    SEH_ReadPtr((char*)pThis + VECCTRL_FOOTHOLD, &petFh);

                    void* userVec = NULL;
                    void* userFh = NULL;
                    void* pComPtr = NULL;
                    if (SEH_ReadPtr((char*)pUser + 0x11A4, &pComPtr) && pComPtr) {
                        userVec = (void*)((char*)pComPtr - 12);
                        if (userVec) SEH_ReadPtr((char*)userVec + VECCTRL_FOOTHOLD, &userFh);
                    }

                    if (g_config.debugPathfind) std::cout << "[SetActive] petFh=" << petFh << " userFh=" << userFh << "\n";

                    if (petFh && userFh) {
                        bool onSamePlatformAsOwner = false;
                        if (petFh == userFh) {
                            onSamePlatformAsOwner = true;
                            if (g_config.debugPathfind) std::cout << "[SetActive] Same foothold segment. Blocking.\n";
                        } else {
                            std::vector<RouteStep> ownerSteps;
                            bool foundRoute = Pathfinder::FindRoute(petFh, userFh, ownerSteps);
                            if (g_config.debugPathfind) std::cout << "[SetActive] FindRoute=" << foundRoute << " steps=" << ownerSteps.size() << "\n";
                            if (foundRoute) {
                                bool allWalk = true;
                                for (size_t i = 0; i < ownerSteps.size(); i++) {
                                    int edgeType = ownerSteps[i].hasEdge ? ownerSteps[i].edge.type : -1;
                                    if (g_config.debugPathfind) std::cout << "  Step " << i << ": type=" << edgeType << "\n";
                                    if (ownerSteps[i].hasEdge && ownerSteps[i].edge.type != EDGE_WALK) {
                                        allWalk = false;
                                    }
                                }
                                if (allWalk) {
                                    onSamePlatformAsOwner = true;
                                    if (g_config.debugPathfind) std::cout << "[SetActive] Walkable path. Blocking.\n";
                                }
                            }
                        }
                        if (onSamePlatformAsOwner) {
                            return;
                        }
                    }
                }
            }
        }
        if (Original_VecCtrlPet_SetActive) {
            Original_VecCtrlPet_SetActive(pThis, active, x, y, vx, vy, action, fh);
        }
    }

    void WritePetAIHook()
    {
        DWORD hAddr = 0x009C4122;
        DWORD old;
        if (!VirtualProtect((LPVOID)hAddr, 5, PAGE_EXECUTE_READWRITE, &old)) return;

        BYTE p[5];
        p[0] = 0xE9;
        DWORD rel = (DWORD)Hooked_WorkUpdateActive - (hAddr + 5);
        p[1] = (BYTE)(rel & 0xFF);
        p[2] = (BYTE)((rel >> 8) & 0xFF);
        p[3] = (BYTE)((rel >> 16) & 0xFF);
        p[4] = (BYTE)((rel >> 24) & 0xFF);

        memcpy((void*)hAddr, p, 5);
        VirtualProtect((LPVOID)hAddr, 5, old, &old);

        DWORD hInitAddr = 0x00529269;
        if (VirtualProtect((LPVOID)hInitAddr, 5, PAGE_EXECUTE_READWRITE, &old)) {
            p[0] = 0xE9;
            rel = (DWORD)Hooked_CField_Init - (hInitAddr + 5);
            p[1] = (BYTE)(rel & 0xFF);
            p[2] = (BYTE)((rel >> 8) & 0xFF);
            p[3] = (BYTE)((rel >> 16) & 0xFF);
            p[4] = (BYTE)((rel >> 24) & 0xFF);
            memcpy((void*)hInitAddr, p, 5);
            VirtualProtect((LPVOID)hInitAddr, 5, old, &old);
        }

        DWORD hVtableAddr = 0x00B3E8AC;
        DWORD* pVtableSlot = (DWORD*)hVtableAddr;
        std::cout << "[PetAI] Vtable slot @ " << (void*)hVtableAddr
                  << " currently = " << (void*)*pVtableSlot << "\n";
        std::cout << "[PetAI] Expected CVecCtrlPet::SetActive = " << (void*)0x9C40E8 << "\n";
        if (*pVtableSlot == 0x9C40E8) {
            if (VirtualProtect((LPVOID)hVtableAddr, 4, PAGE_EXECUTE_READWRITE, &old)) {
                Original_VecCtrlPet_SetActive = *(PFN_SetActive*)hVtableAddr;
                *(PFN_SetActive*)hVtableAddr = (PFN_SetActive)Hooked_VecCtrl_SetActive;
                VirtualProtect((LPVOID)hVtableAddr, 4, old, &old);
                std::cout << "[PetAI] SetActive vtable hook installed OK.\n";
            } else {
                std::cout << "[PetAI] VirtualProtect FAILED for vtable hook!\n";
            }
        } else {
            std::cout << "[PetAI] Vtable slot mismatch! SetActive hook NOT installed.\n";
        }

        std::cout << "[PetAI] Pathfinding hook written.\n";
    }

    // ---------- ApplyPatches -------------------------------------------------------
    void ApplyPatches()
    {
        TargetManager::Init();

        std::vector<BYTE> patchX = { 0xB8, 0x00, 0x00, 0x00, 0x00 };
        patchX[1] = (BYTE)(g_config.telescopePickupRangeX & 0xFF);
        patchX[2] = (BYTE)((g_config.telescopePickupRangeX >> 8) & 0xFF);
        patchX[3] = (BYTE)((g_config.telescopePickupRangeX >> 16) & 0xFF);
        patchX[4] = (BYTE)((g_config.telescopePickupRangeX >> 24) & 0xFF);
        PatchMemory(0x005083fc, patchX);

        BYTE patchYUp = (BYTE)(g_config.telescopePickupRangeYUp & 0xFF);
        PatchMemory(0x00508517 + 2, { patchYUp });

        BYTE patchYDown = (BYTE)(g_config.telescopePickupRangeYDown & 0xFF);
        PatchMemory(0x00508529 + 2, { patchYDown });

        WriteHook();
        WriteUpdatePetAbilityHook();
        WritePetAIHook();
        WriteStringPoolHook();
    }

} // namespace PetHelper

