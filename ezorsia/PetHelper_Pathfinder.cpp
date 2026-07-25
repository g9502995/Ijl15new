#include "stdafx.h"
#include "PetHelper_Internal.h"
#include <queue>
#include <algorithm>

// ===========================================================================
//  Pathfinder - builds a foothold adjacency graph once per map and runs A*
//  over it. Geometry primitives (LoX/MidX/YAtX/...) stay inline in the header
//  since BuildAdjacency calls them O(n^2) times per map load and A* calls
//  Heuristic() on every relaxed edge; everything else lives here.
// ===========================================================================
namespace PetHelper {

    std::vector<FhNode>             Pathfinder::fh;
    std::vector<std::vector<Edge> > Pathfinder::adj;
    std::vector<char>               Pathfinder::ledgeL;
    std::vector<char>               Pathfinder::ledgeR;
    DWORD                           Pathfinder::fhTime = 0;
    void*                           Pathfinder::fhSpace = NULL;
    std::vector<BlacklistEntry>     Pathfinder::blacklist;
    std::vector<LadderNode>         Pathfinder::ropes;

    std::vector<int>  Pathfinder::s_gScore;
    std::vector<int>  Pathfinder::s_parent;
    std::vector<int>  Pathfinder::s_parentEdge;
    std::vector<char> Pathfinder::s_closed;

    bool Pathfinder::LoadFhNode(void* pFh, FhNode& out) {
        if (!pFh) return false;
        int x1, y1, x2, y2, attr;
        if (!SEH_ReadFoothold(pFh, &x1, &y1, &x2, &y2, &attr)) return false;
        if (x1 == x2) return false;              // vertical wall
        out.fh = pFh;
        out.x1 = x1; out.y1 = y1;
        out.x2 = x2; out.y2 = y2;
        return true;
    }

    // Layout from CWvsPhysicalSpace2D::GetFootholdClosest (0xA45681)
    void Pathfinder::EnumFootholds(std::vector<FhNode>& out) {
        out.clear();
        void* pSpace = *g_ppPhysicalSpace;
        if (!pSpace) return;

        void* node = NULL;
        if (!SEH_ReadPtr((char*)pSpace + 0x88, &node)) return;

        for (int i = 0; i < 4096 && node; i++) {
            void* pFh = NULL;
            void* nextRaw = NULL;
            if (!SEH_ReadPtr((char*)node + 4, &pFh))     break;
            if (!SEH_ReadPtr((char*)node - 12, &nextRaw)) break;

            node = nextRaw ? (void*)((char*)nextRaw + 16) : NULL;
            if (!pFh) continue;

            FhNode n;
            if (LoadFhNode(pFh, n)) out.push_back(n);
        }
    }


    bool Pathfinder::AnyEndpointNear(int idx, int x, int y) {
        for (int i = 0; i < (int)fh.size(); i++) {
            if (i == idx) continue;
            int lx, ly, rx, ry;
            LeftPt(fh[i], lx, ly);
            RightPt(fh[i], rx, ry);
            if (abs(lx - x) <= g_config.walkGapTol && abs(ly - y) <= g_config.walkStepTol) return true;
            if (abs(rx - x) <= g_config.walkGapTol && abs(ry - y) <= g_config.walkStepTol) return true;
        }
        return false;
    }

    int Pathfinder::FindFhIndex(void* pFh) {
        for (int i = 0; i < (int)fh.size(); i++)
            if (fh[i].fh == pFh) return i;
        return -1;
    }

    bool Pathfinder::IsLinkBlacklisted(int ia, int ib) {
        for (size_t i = 0; i < blacklist.size(); i++)
            if (blacklist[i].ia == ia && blacklist[i].ib == ib) return true;
        return false;
    }

    void Pathfinder::BlacklistLink(void* pFhA, void* pFhB, DWORD now) {
        int ia = FindFhIndex(pFhA);
        int ib = FindFhIndex(pFhB);
        if (ia >= 0 && ib >= 0) {
            BlacklistEntry e;
            e.ia = ia; e.ib = ib; e.expire = now + 10000;
            blacklist.push_back(e);
        }
    }

    void Pathfinder::RecordEdgeOutcome(int fromIdx, int toIdx, bool success,
        DWORD durationMs, bool logNow) {
        if (fromIdx < 0 || fromIdx >= (int)adj.size()) return;
        std::vector<Edge>& es = adj[fromIdx];
        for (size_t k = 0; k < es.size(); k++) {
            if (es[k].to != toIdx) continue;
            Edge& e = es[k];
            if (success) {
                if (e.failCount > 0) e.failCount--;
                if (e.successCount < 60000) e.successCount++;
                if (durationMs > 0) {
                    e.avgDurationMs = (e.avgDurationMs <= 0.0f)
                        ? (float)durationMs
                        : (e.avgDurationMs * 0.8f + (float)durationMs * 0.2f);
                }
            } else {
                if (e.failCount < 1000) e.failCount++;
                if (e.failureCount < 60000) e.failureCount++;
            }
            if (logNow) {
                std::cout << "[EdgeStat] fh" << fromIdx << "->fh" << toIdx
                    << " ok=" << e.successCount << " bad=" << e.failureCount
                    << " avgMs=" << (int)e.avgDurationMs
                    << " streak=" << e.failCount
                    << " reliabilityCost=" << EdgeReliabilityCost(e) << "\n";
            }
            break;
        }
    }

    // Blends an edge's mid/long-term successCount/failureCount/avgDurationMs
    // into A*'s cost. Kept separate from failCount/edgeFailPenalty (the
    // short-term consecutive-attempt penalty above): that one reacts within
    // a few tries and drives the hard blacklist, this one is a slower-moving
    // preference that only kicks in once an edge has a real track record.
    int Pathfinder::EdgeReliabilityCost(const Edge& e) {
        int total = (int)e.successCount + (int)e.failureCount;
        const int kMinSamples = 4;
        if (total < kMinSamples) return 0; // not enough data yet - don't guess

        double failRate = (double)e.failureCount / (double)total;
        int cost = (int)(failRate * 10.0 * g_config.edgeFailurePenalty);

        // A high average completion time means the pet is fighting this edge
        // (retries, drift correction) even on attempts that eventually count
        // as a success.
        if (e.avgDurationMs > 400.0f)
            cost += (int)((e.avgDurationMs - 400.0f) / 25.0);

        if (failRate < 0.15)
            cost -= g_config.edgeSuccessBonus;

        cost = (int)(cost * (g_config.reliabilityWeight / 100.0));

        // Safety floor: reliability can only ever nudge cost, never make an
        // edge look free or better than a fresh, unproven one by more than
        // the configured bonus.
        int floor = -g_config.edgeSuccessBonus;
        if (cost < floor) cost = floor;
        return cost;
    }

    // ---- edge construction --------------------------------------------------
    // Builds every edge once per map refresh, with the take-off point baked in.
    bool Pathfinder::MakeEdge(int ia, int ib, Edge& out)
    {
        const FhNode& a = fh[ia];
        const FhNode& b = fh[ib];

        int aLo = LoX(a), aHi = HiX(a);
        int bLo = LoX(b), bHi = HiX(b);

        int alx, aly, arx, ary, blx, bly, brx, bry;
        LeftPt(a, alx, aly); RightPt(a, arx, ary);
        LeftPt(b, blx, bly); RightPt(b, brx, bry);

        // 1) WALK - endpoints close enough to step across, small gaps allowed.
        {
            int ax[2] = { alx, arx }, ay[2] = { aly, ary };
            int bx[2] = { blx, brx }, by[2] = { bly, bry };
            for (int i = 0; i < 2; i++) {
                for (int j = 0; j < 2; j++) {
                    if (abs(ax[i] - bx[j]) <= g_config.walkGapTol &&
                        abs(ay[i] - by[j]) <= g_config.walkStepTol) {
                        if (g_config.useWallCheck && !SEH_CanGoThrough(ax[i], ay[i], bx[j], by[j]))
                            continue; // this pair of endpoints is wall-blocked, try the others
                        out.to = ib;
                        out.type = EDGE_WALK;
                        out.takeoffX = ax[i];
                        out.landingX = bx[j];
                        out.cost = g_config.costWalk + abs(MidX(b) - MidX(a)) / 8;
                        return true;
                    }
                }
            }
        }

        // Horizontal gap between the two spans (0 when they overlap).
        int gap = 0;
        if (bLo > aHi)      gap = bLo - aHi;
        else if (aLo > bHi) gap = aLo - bHi;

        // 2) JUMP - jump across to another platform (up, flat, or slight descent)
        {
            int probe = Clamp(MidX(b), aLo, aHi);
            int rise = YAtX(a, probe) - MidY(b);      // positive = b is above, negative = b is below
            if (abs(rise) <= g_config.jumpUpMax && gap <= g_config.jumpReachX) {
                double rr = (double)abs(rise) / (double)g_config.jumpUpMax;
                double gr = (double)gap / (double)g_config.jumpReachX;
                if (rr * rr + gr * gr <= 1.5) {
                    int takeoff = (bLo > aHi) ? aHi : (aLo > bHi ? aLo : Clamp(MidX(b), aLo, aHi));
                    int landing = Clamp(takeoff, bLo, bHi);
                    if (!g_config.useWallCheck || SEH_CanGoThrough(takeoff, YAtX(a, takeoff), landing, YAtX(b, landing))) {
                        out.to = ib;
                        out.type = EDGE_JUMP;
                        out.takeoffX = takeoff;
                        out.landingX = landing;
                        out.cost = g_config.costJump + abs(rise) + gap + 10;
                        return true;
                    }
                }
            }
        }

        // 3) DROP - fall onto something below.
        {
            int probe = Clamp(MidX(b), aLo, aHi);
            int fall = MidY(b) - YAtX(a, probe);      // positive = b is below
            if (fall > g_config.walkStepTol && fall <= g_config.dropDownMax && gap <= g_config.dropReachX) {
                double fr = (double)fall / (double)g_config.dropDownMax;
                double gr = (double)gap / (double)g_config.dropReachX;
                if (fr * fr + gr * gr <= 1.0) {
                    bool overlap = (gap == 0);
                    int  takeoff = -1;

                    if (g_config.useDownjump && overlap) {
                        takeoff = Clamp(MidX(b), aLo, aHi);
                    }
                    else {
                        bool fitL = (bLo - g_config.dropReachX <= aLo && aLo <= bHi + g_config.dropReachX);
                        bool fitR = (bLo - g_config.dropReachX <= aHi && aHi <= bHi + g_config.dropReachX);
                        if (ledgeL[ia] && fitL) takeoff = aLo;
                        else if (ledgeR[ia] && fitR) takeoff = aHi;
                        else if (g_config.useDownjump) takeoff = Clamp(MidX(b), aLo, aHi);
                    }

                    if (takeoff >= 0) {
                        int landing = Clamp(takeoff, bLo, bHi);
                        if (!g_config.useWallCheck || SEH_CanGoThrough(takeoff, YAtX(a, takeoff), landing, YAtX(b, landing))) {
                            out.to = ib;
                            out.type = EDGE_DROP;
                            out.takeoffX = takeoff;
                            out.landingX = landing;
                            out.cost = g_config.costDrop + fall / 16 + gap / 2;
                            return true;
                        }
                    }
                }
            }
        }

        return false;
    }

    void Pathfinder::BuildAdjacency()
    {
        int n = (int)fh.size();
        adj.assign(n, std::vector<Edge>());

        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                if (i == j) continue;
                Edge e;
                if (MakeEdge(i, j, e)) adj[i].push_back(e);
            }
        }
    }

    void Pathfinder::EnumLadders(std::vector<LadderNode>& out)
    {
        out.clear();
        void* pSpace = *g_ppPhysicalSpace;
        if (!pSpace) return;

        void* arrBase = NULL;
        if (!SEH_ReadPtr((char*)pSpace + PHYS_LADDER_ARRAY, &arrBase) || !arrBase) return;

        int count = 0;
        if (!SEH_ReadInt((char*)arrBase - 4, &count)) return;
        if (count <= 1 || count > 2048) return;

        for (int idx = 1; idx < count; idx++) {
            void* entry = (char*)arrBase + idx * 28;
            int x = 0, yTop = 0, yBottom = 0, isLadder = 0;
            if (!SEH_ReadLadderEntry(entry, &x, &yTop, &yBottom, &isLadder)) continue;
            if (yTop > yBottom) { int t = yTop; yTop = yBottom; yBottom = t; }
            LadderNode n;
            n.x = x; n.yTop = yTop; n.yBottom = yBottom; n.isLadder = (isLadder != 0);
            out.push_back(n);
        }
    }

    int Pathfinder::FindFootholdSpanning(int x, int y, int tol)
    {
        int bestIdx = -1;
        int minDiffY = 9999;
        for (int i = 0; i < (int)fh.size(); i++) {
            // Allow 40px horizontal margin around foothold bounds for ladders/ropes attached to edges
            if (x < LoX(fh[i]) - 40 || x > HiX(fh[i]) + 40) continue;
            int fhY = YAtX(fh[i], Clamp(x, LoX(fh[i]), HiX(fh[i])));
            int diffY = abs(fhY - y);
            if (diffY <= tol && diffY < minDiffY) {
                minDiffY = diffY;
                bestIdx = i;
            }
        }
        return bestIdx;
    }

    void Pathfinder::AddRopeEdges()
    {
        std::vector<LadderNode> nodes;
        EnumLadders(nodes);
        ropes = nodes;

        for (size_t r = 0; r < nodes.size(); r++) {
            int top = FindFootholdSpanning(nodes[r].x, nodes[r].yTop, g_config.ropeSnapTol);
            int bot = FindFootholdSpanning(nodes[r].x, nodes[r].yBottom, g_config.ropeSnapTol);
            if (top < 0 || bot < 0 || top == bot) continue;

            int riseCost = (nodes[r].yBottom - nodes[r].yTop);

            Edge up;
            up.to = top; up.type = EDGE_ROPE;
            up.takeoffX = nodes[r].x; up.landingX = nodes[r].x;
            up.cost = g_config.costRope + riseCost / 4;
            adj[bot].push_back(up);

            Edge down;
            down.to = bot; down.type = EDGE_ROPE;
            down.takeoffX = nodes[r].x; down.landingX = nodes[r].x;
            down.cost = g_config.costRope + riseCost / 8;
            adj[top].push_back(down);
        }
    }

    void Pathfinder::RefreshFhCache(DWORD now)
    {
        void* pSpace = *g_ppPhysicalSpace;

        std::vector<FhNode> currentFh;
        EnumFootholds(currentFh);

        bool mapChanged = false;
        if (g_forceMapRebuild) {
            mapChanged = true;
            g_forceMapRebuild = false;
        } else if (pSpace != fhSpace || currentFh.size() != fh.size()) {
            mapChanged = true;
        } else if (!fh.empty() && !currentFh.empty()) {
            if (fh[0].fh != currentFh[0].fh || fh[0].x1 != currentFh[0].x1 || fh[0].y1 != currentFh[0].y1) {
                mapChanged = true;
            }
        }

        if (!mapChanged && !fh.empty()) {
            // O(N) cleanup of expired blacklist entries
            blacklist.erase(
                std::remove_if(blacklist.begin(), blacklist.end(),
                    [now](const BlacklistEntry& e) { return now > e.expire; }),
                blacklist.end()
            );
            return;
        }

        fhSpace = pSpace;
        fhTime = now;
        fh = currentFh;

        ledgeL.assign(fh.size(), 0);
        ledgeR.assign(fh.size(), 0);
        for (int i = 0; i < (int)fh.size(); i++) {
            int lx, ly, rx, ry;
            LeftPt(fh[i], lx, ly);
            RightPt(fh[i], rx, ry);
            ledgeL[i] = AnyEndpointNear(i, lx, ly) ? 0 : 1;
            ledgeR[i] = AnyEndpointNear(i, rx, ry) ? 0 : 1;
        }

        BuildAdjacency();
        AddRopeEdges();

        // O(N) cleanup of expired blacklist entries on map change
        blacklist.erase(
            std::remove_if(blacklist.begin(), blacklist.end(),
                [now](const BlacklistEntry& e) { return now > e.expire; }),
            blacklist.end()
        );

        if (mapChanged) {
            for (int i = 0; i < 3; i++) {
                g_pets[i].Reset();
            }

            // Edge experience (successCount/failureCount/avgDurationMs) is
            // already wiped for free - BuildAdjacency() just replaced every
            // Edge above with a fresh default-constructed one. Target-level
            // experience lives outside the foothold graph though, so it
            // needs an explicit clear here to avoid judging a same-looking
            // coordinate on a new map by an old map's history.
            TargetManager::failHistory.clear();
            TargetSelector::ClearCache();

            if (g_config.dumpMapGraph) {
                char path[64];
                sprintf_s(path, "ezorsia_map_dump_%u.txt", GetCurrentProcessId());
                FILE* fp = NULL;
                fopen_s(&fp, path, "w");
                if (fp) {
                    fprintf(fp, "=== Footholds: %d ===\n", (int)fh.size());
                    for (int i = 0; i < (int)fh.size(); i++) {
                        int lx, ly, rx, ry;
                        LeftPt(fh[i], lx, ly);
                        RightPt(fh[i], rx, ry);
                        fprintf(fp, "FH[%3d] (%6d,%6d)-(%6d,%6d) LedgeL:%d LedgeR:%d edges:%d\n",
                            i, lx, ly, rx, ry, ledgeL[i], ledgeR[i], (int)adj[i].size());
                        for (size_t k = 0; k < adj[i].size(); k++) {
                            const Edge& e = adj[i][k];
                            const char* t = (e.type == EDGE_WALK) ? "WALK"
                                : (e.type == EDGE_JUMP) ? "JUMP"
                                : (e.type == EDGE_DROP) ? "DROP" : "ROPE";
                            fprintf(fp, "        -> FH[%3d] %s takeoff=%d land=%d cost=%d\n",
                                e.to, t, e.takeoffX, e.landingX, e.cost);
                        }
                    }
                    fclose(fp);
                }
            }
        }
    }

    // ---- A* -----------------------------------------------------------------
    bool Pathfinder::FindRoute(void* startFh, void* goalFh, std::vector<RouteStep>& out)
    {
        out.clear();
        if (!startFh || !goalFh) return false;
        if (fh.empty()) return false;

        int s = FindFhIndex(startFh);
        int g = FindFhIndex(goalFh);
        if (s < 0 || g < 0) return false;

        if (s == g) {
            RouteStep st;
            st.fh = fh[s].fh;
            st.hasEdge = false;
            memset(&st.edge, 0, sizeof(st.edge));
            out.push_back(st);
            return true;
        }

        int n = (int)fh.size();

        // Reuse pre-allocated static working buffers to avoid heap allocations
        s_gScore.assign(n, INT_MAX);
        s_parent.assign(n, -1);
        s_parentEdge.assign(n, -1);
        s_closed.assign(n, 0);

        std::priority_queue<PQNode, std::vector<PQNode>, std::greater<PQNode> > pq;

        s_gScore[s] = 0;
        PQNode start; start.idx = s; start.f = Heuristic(s, g);
        pq.push(start);

        int explored = 0;
        while (!pq.empty() && explored < g_config.pathMaxNodes) {
            PQNode cur = pq.top(); pq.pop();
            if (s_closed[cur.idx]) continue;
            s_closed[cur.idx] = 1;
            explored++;

            if (cur.idx == g) break;

            const std::vector<Edge>& es = adj[cur.idx];
            for (size_t k = 0; k < es.size(); k++) {
                const Edge& e = es[k];
                if (s_closed[e.to]) continue;
                if (IsLinkBlacklisted(cur.idx, e.to)) continue;

                int ng = s_gScore[cur.idx] + e.cost + e.failCount * g_config.edgeFailPenalty
                    + EdgeReliabilityCost(e);
                if (ng < s_gScore[e.to]) {
                    s_gScore[e.to] = ng;
                    s_parent[e.to] = cur.idx;
                    s_parentEdge[e.to] = (int)k;
                    PQNode nx; nx.idx = e.to; nx.f = ng + Heuristic(e.to, g);
                    pq.push(nx);
                }
            }
        }

        if (s_gScore[g] == INT_MAX) return false;

        std::vector<int> revNode;
        for (int p = g; p != -1; p = s_parent[p]) revNode.push_back(p);

        for (int i = (int)revNode.size() - 1; i >= 0; i--) {
            int node = revNode[i];
            RouteStep st;
            st.fh = fh[node].fh;
            if (i > 0) {
                int nextNode = revNode[i - 1];
                int ei = s_parentEdge[nextNode];
                st.edge = adj[node][ei];
                st.hasEdge = true;
            }
            else {
                memset(&st.edge, 0, sizeof(st.edge));
                st.hasEdge = false;
            }
            out.push_back(st);
        }
        return true;
    }

    int Pathfinder::RouteCost(const std::vector<RouteStep>& steps)
    {
        int total = 0;
        for (size_t i = 0; i < steps.size(); i++)
            if (steps[i].hasEdge)
                total += steps[i].edge.cost + steps[i].edge.failCount * g_config.edgeFailPenalty
                    + EdgeReliabilityCost(steps[i].edge);
        return total;
    }

} // namespace PetHelper

