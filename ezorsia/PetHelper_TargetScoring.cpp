#include "stdafx.h"
#include "PetHelper_Internal.h"
#include <algorithm>

// ===========================================================================
//  TargetSelector - picks the best drop to chase out of the nearest few
//  candidates using a composite score instead of raw A* cost / distance:
//
//    score = routeCost + unreachablePenalty + contestedPenalty
//                       + recentFailurePenalty - reachableBonus
//
//  A short TTL cache avoids re-running FindRoute for the same (pet, drop)
//  pair on every single frame.
// ===========================================================================
namespace PetHelper {

    std::vector<TargetSelector::ScoreCacheEntry> TargetSelector::s_cache;

    // Fixed, non-configurable nudge that mirrors unreachableTargetPenalty:
    // a reachable candidate is always preferred over an unreachable one, but
    // the two are never simultaneously in play for the same candidate, so
    // this doesn't need its own config knob.
    static const int kReachableBonus = 20;

    void TargetSelector::ClearCache() {
        s_cache.clear();
    }

    bool TargetSelector::GetCachedScore(int petIdx, int x, int y, DWORD now, ScoredCandidate& out) {
        for (size_t i = 0; i < s_cache.size(); i++) {
            const ScoreCacheEntry& c = s_cache[i];
            if (c.petIdx != petIdx || c.x != x || c.y != y) continue;
            if (now - c.time >= g_config.targetScoreCacheMs) continue; // stale - keep looking, don't return it

            out.x = c.x; out.y = c.y;
            out.score = c.score;
            out.routeCost = c.routeCost;
            out.reachable = c.reachable;
            out.contested = c.contested;
            return true;
        }
        return false;
    }

    bool TargetSelector::ScoreTarget(int petIdx, void* startFh, const TargetDrop& d,
        DWORD now, bool logNow, ScoredCandidate& out)
    {
        // startFh must match too: a pet that jumped to a different platform
        // since the entry was cached would otherwise reuse a routeCost that
        // was computed from a start point it isn't standing on anymore.
        for (size_t i = 0; i < s_cache.size(); i++) {
            ScoreCacheEntry& c = s_cache[i];
            if (c.petIdx != petIdx || c.x != d.x || c.y != d.y || c.startFh != startFh) continue;
            if (now - c.time >= g_config.targetScoreCacheMs) break;

            int ownerIdx = -1;
            bool contestedNow = TargetManager::IsLockedByOtherPet(petIdx, d.x, d.y, &ownerIdx);
            
            out.x = c.x; out.y = c.y;
            out.routeCost = c.routeCost;
            out.reachable = c.reachable;
            out.contested = contestedNow;
            
            // Adjust score dynamically if contested state changed since cache creation
            int score = c.score;
            if (contestedNow && !c.contested) {
                score += g_config.contestedTargetPenalty;
            } else if (!contestedNow && c.contested) {
                score -= g_config.contestedTargetPenalty;
            }
            out.score = score;
            return true;
        }

        void* goalFh = SEH_GetFootholdUnder(d.x, d.y - 5, NULL);
        if (!goalFh) goalFh = SEH_GetFootholdClosest(d.x, d.y);

        int  routeCost;
        bool reachable;
        if (!goalFh) {
            routeCost = INT_MAX;
            reachable = false;
        } else if (goalFh == startFh) {
            routeCost = 0;
            reachable = true;
        } else {
            std::vector<RouteStep> tmp;
            if (Pathfinder::FindRoute(startFh, goalFh, tmp)) {
                routeCost = Pathfinder::RouteCost(tmp);
                reachable = true;
            } else {
                routeCost = INT_MAX;
                reachable = false;
            }
        }

        int ownerIdx = -1;
        bool contested = TargetManager::IsLockedByOtherPet(petIdx, d.x, d.y, &ownerIdx);
        int recentFails = TargetManager::RecentFailureCount(d.x, d.y, now);

        int score;
        if (!reachable) {
            score = routeCost >= INT_MAX - g_config.unreachableTargetPenalty
                ? INT_MAX / 2
                : routeCost + g_config.unreachableTargetPenalty;
        } else {
            score = routeCost - kReachableBonus;
        }
        if (contested) score += g_config.contestedTargetPenalty;
        if (recentFails > 0) score += recentFails * g_config.edgeFailurePenalty;

        out.x = d.x; out.y = d.y;
        out.routeCost = routeCost;
        out.score = score;
        out.reachable = reachable;
        out.contested = contested;

        bool updated = false;
        for (size_t i = 0; i < s_cache.size(); i++) {
            if (s_cache[i].petIdx == petIdx && s_cache[i].x == d.x && s_cache[i].y == d.y
                && s_cache[i].startFh == startFh) {
                s_cache[i].score = score;
                s_cache[i].routeCost = routeCost;
                s_cache[i].reachable = reachable;
                s_cache[i].contested = contested;
                s_cache[i].time = now;
                updated = true;
                break;
            }
        }
        if (!updated) {
            ScoreCacheEntry e;
            e.petIdx = petIdx; e.x = d.x; e.y = d.y; e.startFh = startFh;
            e.score = score; e.routeCost = routeCost;
            e.reachable = reachable; e.contested = contested;
            e.time = now;
            s_cache.push_back(e);
        }

        if (logNow) {
            std::cout << "[Score] pet=" << petIdx << " drop=(" << d.x << "," << d.y << ")"
                << " routeCost=" << (reachable ? routeCost : -1)
                << " reachable=" << reachable
                << " contested=" << contested << (contested ? ("(by " + std::to_string(ownerIdx) + ")") : "")
                << " recentFails=" << recentFails
                << " -> score=" << score << "\n";
        }
        return true;
    }

    bool TargetSelector::SelectBestTarget(int petIdx, void* startFh,
        const std::vector<TargetDrop>& drops, DWORD now, bool logNow,
        int& outX, int& outY)
    {
        if (drops.empty() || !startFh) return false;

        // Bound-size cache: nothing frame-persistent grows without limit even
        // across a long play session with many distinct drops.
        s_cache.erase(
            std::remove_if(s_cache.begin(), s_cache.end(),
                [now](const ScoreCacheEntry& c) { return now - c.time > g_config.targetScoreCacheMs * 4; }),
            s_cache.end());

        int considered = (int)drops.size();
        if (considered > g_config.targetCostCandidates) considered = g_config.targetCostCandidates;

        // Score every considered candidate up front before picking, so the
        // "prefer unclaimed" rule below can see the whole set instead of
        // reacting greedily to whichever one happened to be scored first.
        std::vector<ScoredCandidate> scored;
        scored.reserve(considered);
        for (int i = 0; i < considered; i++) {
            ScoredCandidate sc;
            ScoreTarget(petIdx, startFh, drops[i], now, logNow, sc);
            scored.push_back(sc);
        }

        // Pass 1: reachable AND not locked by another pet. If any such
        // candidate exists, pets must divide up the visible drops instead of
        // piling onto the same one - contested candidates aren't even
        // considered here.
        int bestIdx = -1;
        int bestScore = INT_MAX;
        for (int i = 0; i < considered; i++) {
            if (!scored[i].reachable || scored[i].contested) continue;
            if (scored[i].score < bestScore) { bestScore = scored[i].score; bestIdx = i; }
        }

        // Pass 2: every reachable candidate was already claimed by another
        // pet - fall back to the best of those (contestedTargetPenalty,
        // already folded into score, breaks ties among them) rather than
        // idling while something reachable-but-occupied is sitting right
        // there.
        bool usedFallback = false;
        if (bestIdx < 0) {
            for (int i = 0; i < considered; i++) {
                if (!scored[i].reachable) continue;
                if (scored[i].score < bestScore) { bestScore = scored[i].score; bestIdx = i; }
            }
            usedFallback = (bestIdx >= 0);
        }

        if (bestIdx < 0) {
            if (logNow) {
                std::cout << "[Score] pet=" << petIdx << " all " << considered
                    << " candidates unreachable, falling back to owner\n";
            }
            return false;
        }

        outX = drops[bestIdx].x;
        outY = drops[bestIdx].y;
        if (logNow) {
            std::cout << "[Score] pet=" << petIdx << " picked (" << outX << "," << outY
                << ") score=" << bestScore
                << (usedFallback ? " [fallback: all reachable candidates contested]" : "")
                << " among " << considered << " candidates\n";
        }
        return true;
    }

} // namespace PetHelper
