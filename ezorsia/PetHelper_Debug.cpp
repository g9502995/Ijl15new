#include "stdafx.h"
#include "PetHelper_Internal.h"
#include "vendor/imgui/imgui.h"
#include <algorithm>

// ===========================================================================
//  PetHelper_Debug.cpp - in-game ImGui overlay that draws the foothold graph,
//  ropes/ladders, and each pet's current route/target. Toggled with F9.
//  Rendered from Direct3DDevice8::EndScene (see d3d8to9_device.cpp), same
//  place DrawSetItemImGui() runs - so it shares the same ImGui frame.
// ===========================================================================
namespace PetHelper {

    static bool s_showDebug = false;

    static ImU32 PetColor(int idx) {
        static const ImU32 colors[3] = {
            IM_COL32(255, 90, 90, 255),
            IM_COL32(90, 220, 90, 255),
            IM_COL32(110, 170, 255, 255),
        };
        return colors[(idx >= 0 && idx <= 2) ? idx : 0];
    }

    void DrawPetHelperDebugImGui()
    {
        if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) s_showDebug = !s_showDebug;
        if (!s_showDebug) return;

        ImGui::SetNextWindowSize(ImVec2(860, 620), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Pet Pathfinding Debug (F9 to hide)", &s_showDebug)) {
            ImGui::End();
            return;
        }

        for (int i = 0; i < 3; i++) {
            const PetSlotContext& pet = g_pets[i];
            if (!pet.pCPet) { ImGui::TextDisabled("Pet %d: (not active)", i); continue; }
            ImGui::TextColored(ImColor(PetColor(i)), "Pet %d", i);
            ImGui::SameLine();
            ImGui::Text("target=(%d,%d) route steps=%d telescope=%s",
                pet.targetX, pet.targetY, (int)pet.route.steps.size(),
                pet.hasTelescope ? "yes" : "no");

            // Smart-decision v1 stats: last scored candidate for the current
            // target (if still cached), plus success rate across the edges
            // making up the currently planned route.
            if (pet.targetX != -1) {
                TargetSelector::ScoredCandidate sc;
                DWORD nowDbg = GetTickCount();
                if (TargetSelector::GetCachedScore(i, pet.targetX, pet.targetY, nowDbg, sc)) {
                    ImGui::Text("    score=%d routeCost=%d reachable=%s contested=%s",
                        sc.score, sc.routeCost, sc.reachable ? "yes" : "no",
                        sc.contested ? "yes" : "no");
                } else {
                    ImGui::TextDisabled("    (no cached score yet)");
                }
            }

            if (pet.route.valid && !pet.route.steps.empty()) {
                int edgeCount = 0, sumSuccess = 0, sumFail = 0;
                int routeCost = Pathfinder::RouteCost(pet.route.steps);
                for (size_t k = 0; k < pet.route.steps.size(); k++) {
                    if (!pet.route.steps[k].hasEdge) continue;
                    const Edge& e = pet.route.steps[k].edge;
                    edgeCount++;
                    sumSuccess += e.successCount;
                    sumFail += e.failureCount;
                }
                if (edgeCount > 0) {
                    int totalAttempts = sumSuccess + sumFail;
                    if (totalAttempts > 0) {
                        double rate = 100.0 * sumSuccess / totalAttempts;
                        ImGui::Text("    route cost=%d edges=%d successRate=%.0f%% (%d/%d)",
                            routeCost, edgeCount, rate, sumSuccess, totalAttempts);
                    } else {
                        ImGui::Text("    route cost=%d edges=%d (no history yet)", routeCost, edgeCount);
                    }
                }
            }
        }
        ImGui::Separator();

        if (Pathfinder::fh.empty()) {
            ImGui::TextUnformatted("No foothold graph yet (need an active, primed pet with telescope).");
            ImGui::End();
            return;
        }

        // ---- world bounding box over everything we're about to draw ---------
        int minX = INT_MAX, maxX = INT_MIN, minY = INT_MAX, maxY = INT_MIN;
        auto expand = [&](int x, int y) {
            if (x < minX) minX = x; if (x > maxX) maxX = x;
            if (y < minY) minY = y; if (y > maxY) maxY = y;
        };
        for (size_t i = 0; i < Pathfinder::fh.size(); i++) {
            expand(Pathfinder::fh[i].x1, Pathfinder::fh[i].y1);
            expand(Pathfinder::fh[i].x2, Pathfinder::fh[i].y2);
        }
        for (size_t i = 0; i < Pathfinder::ropes.size(); i++) {
            expand(Pathfinder::ropes[i].x, Pathfinder::ropes[i].yTop);
            expand(Pathfinder::ropes[i].x, Pathfinder::ropes[i].yBottom);
        }
        for (int i = 0; i < 3; i++) {
            if (!g_pets[i].pCPet) continue;
            if (g_pets[i].targetX != -1) expand(g_pets[i].targetX, g_pets[i].targetY);
        }
        if (minX > maxX) { ImGui::End(); return; }

        int worldW = (std::max)(1, maxX - minX);
        int worldH = (std::max)(1, maxY - minY);

        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        canvasSize.y = (std::max)(canvasSize.y, 200.0f);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + canvasSize.x, p0.y + canvasSize.y);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(15, 15, 20, 255));
        dl->PushClipRect(p0, p1, true);

        const float pad = 20.0f;
        float scale = (std::min)((canvasSize.x - pad * 2) / worldW, (canvasSize.y - pad * 2) / worldH);
        if (!(scale > 0.0f)) scale = 1.0f;

        auto toScreen = [&](int x, int y) -> ImVec2 {
            return ImVec2(p0.x + pad + (x - minX) * scale,
                           p0.y + pad + (y - minY) * scale);
        };

        // footholds
        for (size_t i = 0; i < Pathfinder::fh.size(); i++) {
            const FhNode& n = Pathfinder::fh[i];
            dl->AddLine(toScreen(n.x1, n.y1), toScreen(n.x2, n.y2), IM_COL32(180, 180, 190, 200), 2.0f);
        }
        // ropes (cyan) / ladders (yellow)
        for (size_t i = 0; i < Pathfinder::ropes.size(); i++) {
            const LadderNode& r = Pathfinder::ropes[i];
            ImU32 col = r.isLadder ? IM_COL32(230, 200, 90, 200) : IM_COL32(90, 200, 230, 200);
            dl->AddLine(toScreen(r.x, r.yTop), toScreen(r.x, r.yBottom), col, 1.5f);
        }

        // edges the reliability learning has penalized (orange, brighter = more fails)
        for (size_t i = 0; i < Pathfinder::adj.size(); i++) {
            const std::vector<Edge>& es = Pathfinder::adj[i];
            for (size_t k = 0; k < es.size(); k++) {
                const Edge& e = es[k];
                if (e.failCount <= 0) continue;
                if (e.to < 0 || e.to >= (int)Pathfinder::fh.size()) continue;
                int ay = Pathfinder::YAtX(Pathfinder::fh[i], e.takeoffX);
                int by = Pathfinder::YAtX(Pathfinder::fh[e.to], e.landingX);
                float alpha = (std::min)(1.0f, 0.3f + 0.2f * e.failCount);
                dl->AddLine(toScreen(e.takeoffX, ay), toScreen(e.landingX, by),
                    IM_COL32(255, 140, 0, (int)(alpha * 255)), 2.0f);
            }
        }

        // per-pet: planned route, live position, locked target
        for (int i = 0; i < 3; i++) {
            const PetSlotContext& pet = g_pets[i];
            if (!pet.pCPet) continue;
            ImU32 col = PetColor(i);

            if (pet.route.valid) {
                for (size_t k = 0; k < pet.route.steps.size(); k++) {
                    const RouteStep& step = pet.route.steps[k];
                    if (!step.hasEdge) continue;
                    int fromIdx = Pathfinder::FindFhIndex(step.fh);
                    int toIdx = step.edge.to;
                    if (fromIdx < 0 || toIdx < 0 || toIdx >= (int)Pathfinder::fh.size()) continue;
                    int ax = step.edge.takeoffX, ay = Pathfinder::YAtX(Pathfinder::fh[fromIdx], ax);
                    int bx = step.edge.landingX, by = Pathfinder::YAtX(Pathfinder::fh[toIdx], bx);
                    dl->AddLine(toScreen(ax, ay), toScreen(bx, by), col, 3.0f);
                }
            }

            int petX = 0, petY = 0;
            bool gotPos = SEH_GetObjPos(pet.pCPet, &petX, &petY);
            ImVec2 c{};
            if (gotPos) {
                c = toScreen(petX, petY);
                dl->AddCircleFilled(c, 5.0f, col);
                char label[8];
                sprintf_s(label, "P%d", i);
                dl->AddText(ImVec2(c.x + 6, c.y - 14), col, label);
            }

            if (pet.targetX != -1) {
                ImVec2 t = toScreen(pet.targetX, pet.targetY);
                if (gotPos) dl->AddLine(c, t, col, 1.0f);
                dl->AddCircle(t, 6.0f, col, 12, 2.0f);
            }
        }

        dl->PopClipRect();
        ImGui::Dummy(canvasSize);

        ImGui::Separator();
        ImGui::TextColored(ImColor(IM_COL32(180, 180, 190, 255)), "gray = foothold");
        ImGui::SameLine();
        ImGui::TextColored(ImColor(IM_COL32(90, 200, 230, 255)), "cyan = rope");
        ImGui::SameLine();
        ImGui::TextColored(ImColor(IM_COL32(230, 200, 90, 255)), "yellow = ladder");
        ImGui::SameLine();
        ImGui::TextColored(ImColor(IM_COL32(255, 140, 0, 255)), "orange = penalized edge");
        ImGui::SameLine();
        ImGui::TextUnformatted("| dot = pet pos, ring = locked target, thick line = planned route");

        ImGui::End();
    }

} // namespace PetHelper
