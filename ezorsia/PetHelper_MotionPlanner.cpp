#include "stdafx.h"
#include "PetHelper_Internal.h"

namespace PetHelper {

    SteeringOutput MotionPlanner::Compute(
        int petX, int petY,
        bool isAirborne,
        const RouteStep& step,
        int targetFinalX,
        DWORD now,
        PetSlotContext& ctx
    ) {
        SteeringOutput s;
        s.dir = 0;
        s.kind = 0;
        s.overrideX = -9999;
        s.isAirborneCorrection = false;

        // ---- 1. Airborne correction: predict 2 frames ahead and brake if overshooting ----
        if (isAirborne && ctx.pendingEdgeToIdx >= 0 && step.hasEdge) {
            static int prevX[3] = {};
            int pIdx = -1;
            if (&ctx == &g_pets[0]) pIdx = 0;
            else if (&ctx == &g_pets[1]) pIdx = 1;
            else if (&ctx == &g_pets[2]) pIdx = 2;

            if (pIdx >= 0) {
                int vx = petX - prevX[pIdx];
                prevX[pIdx] = petX;
                int predictedX = petX + vx * 2;
                int targetLandX = step.edge.landingX;
                if (abs(predictedX - targetLandX) > 12) {
                    s.dir = (predictedX > targetLandX) ? -1 : 1;
                    s.isAirborneCorrection = true;
                    s.kind = 0;
                    return s;
                }
            }
        }

        // ---- 2. Takeoff alignment: snap to exact takeoff pixel before jumping ----
        if (!isAirborne && step.hasEdge &&
            (step.edge.type == EDGE_JUMP || step.edge.type == EDGE_DROP)) {
            int tkX = step.edge.takeoffX;
            int dist = tkX - petX;
            int jdir = (step.edge.landingX > step.edge.takeoffX) ? 1 : -1;
            if (step.edge.type == EDGE_DROP) {
                jdir = (dist > 0) ? 1 : -1;
                if (dist == 0) jdir = 0;
            }
            // Already overshot the takeoff: brake back
            if ((jdir == 1 && dist < -5) || (jdir == -1 && dist > 5)) {
                if (ctx.pendingEdgeToIdx == -1) {
                    s.dir = (jdir == 1) ? -1 : 1;
                    s.kind = 0;
                    return s;
                }
            }
            // Within 15px of takeoff: pin position via overrideX
            if ((jdir == 1 && dist > 0 && dist < 15) ||
                (jdir == -1 && dist < 0 && dist > -15)) {
                s.overrideX = tkX;
                s.dir = 0;
                s.kind = 0;
                return s;
            }
        }

        // ---- 3. SteerTo: walk towards goal ----
        int wantX = step.hasEdge ? step.edge.takeoffX : targetFinalX;
        if (step.hasEdge && step.edge.type == EDGE_WALK)
            wantX = step.edge.landingX;
        s.dir = PetController::SteerTo(ctx, petX, wantX);

        // ---- 4. Trigger jump/drop/rope action ----
        if (step.hasEdge) {
            bool atTakeoff = false;
            if (step.edge.type == EDGE_JUMP && step.edge.landingX != step.edge.takeoffX) {
                int jdir2 = (step.edge.landingX > step.edge.takeoffX) ? 1 : -1;
                int ahead = (petX - step.edge.takeoffX) * jdir2;
                atTakeoff = (ahead >= -2) && (ahead <= g_config.actionMargin);
            } else {
                atTakeoff = (abs(petX - step.edge.takeoffX) <= g_config.actionMargin);
            }

            bool forceJump = false;
            DWORD stuckFor = now - ctx.lastMoveT;
            if (stuckFor >= g_config.stuckMs && now - ctx.lastJumpTime > 800)
                forceJump = true;

            if (step.edge.type == EDGE_ROPE) {
                if (!isAirborne && (atTakeoff || forceJump))
                    s.kind = 3; // signal rope climb
            } else if (step.edge.type == EDGE_JUMP) {
                if (!isAirborne && atTakeoff)
                    s.kind = 1;
                else if (!isAirborne && forceJump)
                    s.kind = 1;
            } else if (step.edge.type == EDGE_DROP) {
                if (!isAirborne && atTakeoff) {
                    bool below = (abs(step.edge.landingX - step.edge.takeoffX) <= g_config.aimTol);
                    s.kind = (g_config.useDownjump && below) ? 2 : 0;
                    if (s.kind != 2) wantX = step.edge.landingX;
                } else if (!isAirborne && forceJump) {
                    s.kind = 1;
                }
            } else {
                if (!isAirborne && forceJump) s.kind = 1;
            }
        } else {
            DWORD stuckFor = now - ctx.lastMoveT;
            if (stuckFor >= g_config.stuckMs && now - ctx.lastJumpTime > 800)
                if (!isAirborne) s.kind = 1;
        }

        // Aim direction for committed edge
        bool commitEdge = (s.kind != 0 && step.hasEdge &&
            (step.edge.type == EDGE_JUMP ||
             (step.edge.type == EDGE_DROP && s.kind == 2)));

        if (commitEdge) {
            int aim = step.edge.landingX;
            bool diagonal = abs(step.edge.landingX - step.edge.takeoffX) > g_config.aimTol;
            if (diagonal || abs(aim - petX) > g_config.aimTol)
                s.dir = (aim >= petX) ? 1 : -1;
            else
                s.dir = 0;
        } else if (s.kind != 0 && s.dir == 0 &&
                   !(step.hasEdge && step.edge.type == EDGE_JUMP)) {
            int nudge = step.hasEdge ? step.edge.landingX : targetFinalX;
            s.dir = (nudge >= petX) ? 1 : -1;
        }

        return s;
    }

} // namespace PetHelper
