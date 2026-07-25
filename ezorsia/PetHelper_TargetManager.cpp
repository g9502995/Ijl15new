#include "stdafx.h"
#include "PetHelper_Internal.h"
#include <algorithm>

// ===========================================================================
//  TargetManager - which drop each pet is chasing, and which drops/links to
//  avoid because a pet already proved it can't reach or pick them up.
// ===========================================================================
namespace PetHelper {

    int TargetManager::currentPetIdx = -1;
    int TargetManager::poolScanned = 0;
    std::vector<DropBlacklistEntry> TargetManager::dropBlacklist;

    void TargetManager::Init() {
        for (int i = 0; i < 3; i++) {
            g_pets[i].Reset();
        }
    }

    void TargetManager::BlacklistDrop(int x, int y, DWORD now, DWORD durationMs) {
        DWORD d = durationMs ? durationMs : g_config.dropBlacklistMs;
        DWORD newExpire = (d == DROP_BLACKLIST_FOREVER) ? DROP_BLACKLIST_FOREVER : now + d;

        // A pet lingering near an already-blacklisted spot (e.g. the edge of
        // a pit) can re-trigger this every frame - refresh the existing
        // entry instead of piling up duplicates that a FOREVER entry would
        // otherwise never let the TTL cleanup prune.
        for (size_t i = 0; i < dropBlacklist.size(); i++) {
            if (dropBlacklist[i].x == x && dropBlacklist[i].y == y) {
                if (newExpire == DROP_BLACKLIST_FOREVER || newExpire > dropBlacklist[i].expire)
                    dropBlacklist[i].expire = newExpire;
                return;
            }
        }

        DropBlacklistEntry e;
        e.x = x; e.y = y; e.expire = newExpire;
        dropBlacklist.push_back(e);
    }

    bool TargetManager::IsBlacklisted(int x, int y) {
        for (size_t i = 0; i < dropBlacklist.size(); i++)
            if (dropBlacklist[i].x == x && dropBlacklist[i].y == y) return true;
        return false;
    }

    // Returns false when the item is confirmed unpickable and has been blacklisted.
    bool TargetManager::CheckUnpickable(int petIdx, int petX, int petY,
        int tx, int ty, DWORD now, bool logNow)
    {
        if (petIdx < 0 || petIdx > 2) return true;
        PetSlotContext& pet = g_pets[petIdx];

        if (abs(petX - tx) <= 50 && abs(petY - ty) <= 80) {
            if (abs(pet.onTopTargetX - tx) <= 15 &&
                abs(pet.onTopTargetY - ty) <= 15) {
                // Pet has been sitting within 50/80 px of this item for > 2500 ms
                // without the game picking it up → treat as unreachable.
                if (now - pet.onTopSince > 2500) {
                    BlacklistDrop(tx, ty, now);
                    ClearTarget(petIdx);
                    pet.route.valid = false;
                    pet.onTopTargetX = -1;
                    pet.onTopTargetY = -1;
                    if (logNow) std::cout << "[T] unpickable, skip to next target\n";
                    return false;
                }
            }
            else {
                // First time entering the "on top" zone for this item: start timer.
                pet.onTopTargetX = tx;
                pet.onTopTargetY = ty;
                pet.onTopSince = now;
            }
        }
        else {
            pet.onTopTargetX = -1;
            pet.onTopTargetY = -1;
        }
        return true;
    }


    void TargetManager::LockTarget(int petIdx, int tx, int ty, DWORD now) {
        if (petIdx < 0 || petIdx > 2) return;
        PetSlotContext& pet = g_pets[petIdx];

        // Same tolerance as the lookup, otherwise a drop settling by a couple of
        // pixels counts as a brand new target and resets the commit timer.
        if (pet.targetX != -1 &&
            abs(pet.targetX - tx) <= 10 &&
            abs(pet.targetY - ty) <= 10) {
            pet.targetX = tx;          // refresh coords, keep the timer
            pet.targetY = ty;
        }
        else {
            pet.targetX = tx;
            pet.targetY = ty;
            pet.lockSeen = now;
        }
    }

    void TargetManager::ClearTarget(int petIdx) {
        if (petIdx < 0 || petIdx > 2) return;
        g_pets[petIdx].targetX = -1;
        g_pets[petIdx].targetY = -1;
    }

    void TargetManager::ExpireLock(int petIdx, DWORD now) {
        if (petIdx < 0 || petIdx > 2) return;
        PetSlotContext& pet = g_pets[petIdx];

        if (pet.targetX != -1 && now - pet.lockSeen > 1000)
            ClearTarget(petIdx);
    }

    std::vector<TargetDrop> TargetManager::CollectSortedDrops(int fromX, int fromY,
        bool logNow, DWORD now)
    {
        // O(N) cleanup of expired drop blacklist entries
        dropBlacklist.erase(
            std::remove_if(dropBlacklist.begin(), dropBlacklist.end(),
                [now](const DropBlacklistEntry& e) { return now > e.expire; }),
            dropBlacklist.end()
        );

        std::vector<TargetDrop> drops;
        // Drops another pet already claimed: kept separate so pets divide up
        // the visible drops instead of piling onto the same nearest one, but
        // still usable as a fallback if nothing else is in range.
        std::vector<TargetDrop> claimedFallback;

        void* pPool = *g_ppDropPool;
        if (!pPool) return drops;

        void* node = NULL;
        if (!SEH_ReadPtr((char*)pPool + 0x2C, &node)) return drops;

        int  scanned = 0;
        bool hasLocked = (currentPetIdx >= 0 && currentPetIdx <= 2 &&
            g_pets[currentPetIdx].targetX != -1);
        bool lockedFound = false;
        TargetDrop lockedTd;
        lockedTd.x = lockedTd.y = lockedTd.dist = 0;

        for (int i = 0; i < 1024 && node; i++) {
            void* pDrop = NULL;
            void* nextRaw = NULL;
            if (!SEH_ReadPtr((char*)node + 4, &pDrop))   break;
            if (!SEH_ReadPtr((char*)node - 12, &nextRaw)) break;

            node = nextRaw ? (void*)((char*)nextRaw + 16) : NULL;
            if (!pDrop) continue;
            scanned++;

            int dropX = 0, dropY = 0, dropType = 0;
            if (!SEH_GetDropInfo(pDrop, &dropX, &dropY, &dropType)) continue;
            if (g_config.onlyNormalItems && dropType != 3) continue;

            int dx = abs(dropX - fromX);
            int dy = abs(dropY - fromY);
            if (dy > g_config.maxSearchDistanceY) continue;
            if (dx > g_config.maxSearchDistanceX) continue;

            if (IsBlacklisted(dropX, dropY) && (dx > 80 || dy > 80)) continue;

            // Not actually resting on a platform (fell into a pit / off the
            // side of the map) - a pet can only ever stand near its X on
            // whatever foothold happens to be underneath, never truly reach
            // it. Skip and blacklist so we don't keep re-evaluating it.
            {
                int restCy = 0;
                void* restFh = SEH_GetFootholdUnder(dropX, dropY - 5, &restCy);
                if (!restFh || abs(restCy - dropY) > g_config.dropRestTol) {
                    BlacklistDrop(dropX, dropY, now, DROP_BLACKLIST_FOREVER);
                    continue;
                }
            }

            if (hasLocked &&
                abs(dropX - g_pets[currentPetIdx].targetX) <= 10 &&
                abs(dropY - g_pets[currentPetIdx].targetY) <= 10) {
                lockedTd.x = dropX; lockedTd.y = dropY; lockedTd.dist = dx;
                lockedFound = true;
                g_pets[currentPetIdx].lockSeen = now;
                continue;
            }

            bool claimedByOther = false;
            for (int p = 0; p < 3; p++) {
                if (p != currentPetIdx && g_pets[p].targetX == dropX && g_pets[p].targetY == dropY) {
                    claimedByOther = true;
                    break;
                }
            }

            TargetDrop td;
            td.x = dropX; td.y = dropY; td.dist = dx;
            if (claimedByOther) claimedFallback.push_back(td);
            else drops.push_back(td);
        }

        if (hasLocked && !lockedFound && scanned > 0) {
            ClearTarget(currentPetIdx);
        }
        std::sort(drops.begin(), drops.end(),
            [](const TargetDrop& a, const TargetDrop& b) { return a.dist < b.dist; });

        // Nothing unclaimed in range: fall back to a claimed drop rather than
        // idling, so a lone pet still cleans up when the other two happen to
        // have everything else locked.
        if (drops.empty() && !claimedFallback.empty()) {
            std::sort(claimedFallback.begin(), claimedFallback.end(),
                [](const TargetDrop& a, const TargetDrop& b) { return a.dist < b.dist; });
            drops = claimedFallback;
        }

        if (lockedFound) drops.insert(drops.begin(), lockedTd);

        if (logNow) {
            std::cout << "[T] pool scanned=" << scanned
                << " inRange=" << drops.size() << "\n";
        }

        poolScanned = scanned;
        return drops;
    }

} // namespace PetHelper

