#pragma once

// ===========================================================================
//  Shared internals for the PetHelper feature.
//
//  PetHelper.cpp used to be a single ~1900-line file. It is now split by
//  responsibility:
//    PetHelper_SEH.cpp           - raw memory-read primitives (SEH wrapped)
//    PetHelper_TargetManager.cpp - drop scanning / per-pet target locking
//    PetHelper_Pathfinder.cpp    - foothold graph + A*
//    PetHelper_Controller.cpp    - turns a route step into stick input
//    PetHelper_MiscHooks.cpp     - unrelated small hooks (bounding box,
//                                  telescope ability cache, chat-string hook)
//    PetHelper.cpp               - per-frame AI orchestration + hook install
//
//  This header is the only thing they all share: game addresses/offsets,
//  the graph data types, and the class declarations. Method bodies live in
//  the matching .cpp so each translation unit only pulls in what it needs.
// ===========================================================================
#include <vector>
#include <windows.h>

// ---------------------------------------------------------------------------
//  Global typedefs / game addresses
// ---------------------------------------------------------------------------
typedef long(__thiscall* PFN_GetDataLong)(const void*);
typedef void* (__thiscall* PFN_GetPos)     (void*);
typedef void(__thiscall* PFN_SetInput)   (void* pThis, int nInputX, int nInputY);
typedef void* (__thiscall* PFN_GetFootholdUnderneath)(void* pSpace, int x, int y, int* pcy, int yMin, int nRangeX);
typedef void* (__thiscall* PFN_GetFootholdClosest)   (void* pSpace, int x, int y);
typedef long(__cdecl* PFN_ZtlSecureFuse)(const long* key, unsigned int enc);

static const PFN_SetInput              g_SetInput = (PFN_SetInput)0x9B7B4A;
static const PFN_GetFootholdUnderneath g_GetFootholdUnderneath = (PFN_GetFootholdUnderneath)0xA45585;
static const PFN_GetFootholdClosest    g_GetFootholdClosest = (PFN_GetFootholdClosest)0xA45677;
static const PFN_ZtlSecureFuse         g_ZtlSecureFuse = (PFN_ZtlSecureFuse)0x416563;

// CWvsPhysicalSpace2D::CanGoThrough (0xA457D0) - the same line-of-sight test
// the client uses to validate Teleport/ranged-skill targets: does a straight
// line from (xm1,ym1) to (*xm2,*ym2) cross a solid foothold segment (walls
// included - our own `fh` list drops vertical segments as unwalkable, but the
// engine's live foothold list still has them, so this catches what our
// endpoint-proximity heuristic can't see). Overwrites *xm2/*ym2 with the
// crossing point when blocked; we only care about the bool.
// nZMass gates which candidates even get tested (see MakeEdge/CanGoThrough
// callers TryRegisterTeleport etc.): 0 only matches foothold segments whose
// own group field is 0, i.e. plain default-group terrain - good enough to
// catch ordinary walls without risking a wrong group value rejecting
// everything.
typedef int(__thiscall* PFN_CanGoThrough)(void* pSpace, int xm1, int ym1,
    int* xm2, int* ym2, int nZMass);
static const PFN_CanGoThrough g_CanGoThrough = (PFN_CanGoThrough)0xA457D0;

// CVecCtrlPet::SetActive (0x9C40E8) - retn 1Ch, 7 stack args.
//   SetActive(bActive, x, y, vx, vy, nMoveAction, pFoothold)
// pFoothold = NULL detaches the pet, so a positive vy makes it fall: that is
// how we synthesise a down-jump the engine never gave pets.
typedef void(__thiscall* PFN_SetActive)(void* pThis, int bActive,
    int x, int y, int vx, int vy,
    int nMoveAction, void* pFoothold);
static const PFN_SetActive g_SetActive = (PFN_SetActive)0x9C40E8;

// CPet::OnResolveMoveAction (0x70458C) only ever returns the hang/climb pose
// (0x1E, ORed with a direction-parity bit from the CVecCtrlPet) when BOTH the
// pet's own rope pointer AND the owner's rope pointer are attached - i.e. the
// stock logic only ever shows a pet hanging while mirroring the owner's own
// climb. For a pet climbing on its own (owner not on a rope) that branch is
// unreachable no matter what gets attached natively. CVecCtrl::SetActive
// bypasses that auto-calc entirely when nMoveAction is >= 2 (used as a literal
// display action instead) - CPet::Update copies it into the sprite state every
// frame - so passing this literal directly is what DownJump already does with
// action 4/5 for the down-jump pose.
static const int PET_ACTION_HANG = 0x1E;

// CUser::IsOnLadderOrRope (0x4ACD7B) - const thiscall, no args. Safe read-only
// query (just resolves the same secured rope pointer IsOnRope/OnResolveMoveAction
// use, without mutating anything). Used to detect the owner climbing a rope so
// our own AI can step aside: the native WorkUpdateActive loop (runs every frame
// regardless of our hook) auto-attaches and rides the pet along on its own once
// it sees the owner on a rope and the pet not yet attached - our manual
// SetActive calls fighting it every frame is what produces the sliding glitch.
typedef BOOL(__thiscall* PFN_IsOnLadderOrRope)(void* pThis);
static const PFN_IsOnLadderOrRope g_UserIsOnLadderOrRope =
    (PFN_IsOnLadderOrRope)0x4ACD7B;

// CVecCtrl::WorkUpdateActive - BASE class physics step. The pet subclass calls
// this at the end of its own WorkUpdateActive; calling it directly lets us
// drive the pet without the stock pet AI overwriting our input.
typedef int(__thiscall* PFN_BaseWorkUpdateActive)(void* pThis, int tElapse);
static const PFN_BaseWorkUpdateActive g_BaseWorkUpdateActive =
(PFN_BaseWorkUpdateActive)0x9B19D0;

// CPet chat bubble
struct Ztl_bstr_t { void* pData; };
typedef void(__thiscall* PFN_bstr_t_ctor)(Ztl_bstr_t* pThis, const char* str);
typedef void(__thiscall* PFN_CPet_DoAction)(void* pThis, int action, int bSetPos,
    Ztl_bstr_t sChat, int bSend, DWORD tCur);
static const PFN_bstr_t_ctor   g_bstr_ctor = (PFN_bstr_t_ctor)0x406301;
static       PFN_CPet_DoAction g_PetDoAction = (PFN_CPet_DoAction)0x007055E2;

static void** const g_ppPhysicalSpace = (void**)0x00BEBFA0;
static void** const g_ppDropPool = (void**)0x00BED6AC;
// CWvsPhysicalSpace2D::Load (0xA43E7B, ladderRope branch) / GetLadderOrRope
// (0xA45B03): +0xA8 is a plain array (not a linked list like footholds), one
// DWORD count at [ptr-4], 28-byte entries starting at index 1.
#define PHYS_LADDER_ARRAY 0xA8
// CVecCtrl::WorkUpdateActive (0x9B19D0): cmp [ebx+168h],0 / jz / call Jump
#define VECCTRL_JUMP_FLAG   0x168
// CVecCtrl current foothold pointer (JustJump reads *(this+68) == +0x110)
#define VECCTRL_FOOTHOLD    0x110
// CUser -> CVecCtrlUser
#define CUSER_VECCTRL       0x148
// CPet -> owner CUser (game reads it at 0x9C414D)
#define CPET_OWNER          0x94
// CPet's own pet-slot index (0-2), the same index cachedHasTelescope[] uses.
#define CPET_SLOT_INDEX     0x9C

// CPet::UpdatePetAbility (0x705E2C):
//   *(this + 85) = _ZtlSecureTear<int>((flags & 8) != 0, this + 83);
#define PET_ABIL_KEY  0x14C
#define PET_ABIL_ENC  0x154

// CStaticFoothold layout (confirmed by runtime dump)
#define FH_X1(fh)   (*(int*)((char*)(fh) + 0x0C))
#define FH_Y1(fh)   (*(int*)((char*)(fh) + 0x10))
#define FH_X2(fh)   (*(int*)((char*)(fh) + 0x14))
#define FH_Y2(fh)   (*(int*)((char*)(fh) + 0x18))
#define FH_ATTR(fh) (*(int*)((char*)(fh) + 0x48))

// ---------------------------------------------------------------------------
//  SEH helpers (global scope, no C++ destructor objects inside).
//  Bodies live in PetHelper_SEH.cpp.
// ---------------------------------------------------------------------------
__declspec(noinline) BOOL  SEH_GetObjPos(void* obj, int* pOutX, int* pOutY);
__declspec(noinline) BOOL  SEH_ReadPetAbility(void* pPet, int* out);
__declspec(noinline) BOOL  SEH_GetDropInfo(void* pDropObj, int* pOutX, int* pOutY, int* pOutType);
__declspec(noinline) void* SEH_GetFootholdClosest(int x, int y);
__declspec(noinline) void* SEH_GetFootholdUnder(int x, int y, int* pcy);
__declspec(noinline) BOOL  SEH_ReadFoothold(void* fh, int* x1, int* y1, int* x2, int* y2, int* attr);
// Ladder/rope entry fields (confirmed from CWvsPhysicalSpace2D::Load, the
// ladderRope parse loop): l@0x04, x@0x0C, y1(top)@0x10, y2(bottom)@0x14.
__declspec(noinline) BOOL  SEH_ReadLadderEntry(void* pEntry, int* x, int* yTop, int* yBottom, int* isLadder);
// Fails OPEN (returns TRUE, "unobstructed") on a missing space or an SEH
// fault: an unreliable wall check must never be able to make every edge
// look blocked, only ever narrow down edges that were already accepted.
__declspec(noinline) BOOL  SEH_CanGoThrough(int x1, int y1, int x2, int y2);
__declspec(noinline) BOOL  SEH_ReadPtr(void* p, void** out);
__declspec(noinline) BOOL  SEH_ReadInt(void* p, int* out);
__declspec(noinline) BOOL  SEH_IsValidFoothold(void* fh);
__declspec(noinline) BOOL  SEH_ScalePhysics(double* t, double mult, double* outSpeed, double* outForce);
__declspec(noinline) void  SEH_RestorePhysics(double* t, double speed, double force);

// ===========================================================================
namespace PetHelper {
    // ===========================================================================

    // Centralized configuration structure
    struct PetHelperConfig {
        int   maxSearchDistanceX = 800;
        int   maxSearchDistanceY = 400;
        int   autoPickupThreshold = 275;

        // Drop type filter. 3 = normal item. Set false to also chase meso / etc.
        bool  onlyNormalItems = false;

        // How far a drop's Y may sit from the foothold surface directly
        // beneath it and still count as "resting on a platform". Items that
        // fell into a pit / off the side of the map land far from any
        // foothold at their own X and are filtered out instead of being
        // chased forever.
        int   dropRestTol = 60;

        // How long a rejected drop stays blacklisted before it's considered
        // as a target again. Needs to comfortably outlast how long an item
        // sits on the ground, or a pet just re-discovers the same failure
        // in a loop until the item finally despawns on its own.
        DWORD dropBlacklistMs = 60000;      // generic give-up / edge-fail rejection (1 minute)

        int   telescopePickupRangeX = 688;
        int   telescopePickupRangeYUp = 128;
        int   telescopePickupRangeYDown = 127;

        int   limitPullbackLeft = -800;
        int   limitPullbackRight = 800;
        int   limitPullbackTop = -300;
        int   limitPullbackBottom = 300;
        DWORD noDropGraceMs = 300;

        // ---------- Graph construction -------------------------------------------------
        int   walkGapTol = 25;   // endpoints this far apart still count as walkable
        int   walkStepTol = 18;   // vertical step a walk link may absorb
        int   jumpUpMax = 65;  // max height one jump can climb
        int   jumpReachX = 60;   // max horizontal gap a jump can cross
        int   dropDownMax = 450;  // max height the pet may fall
        int   dropReachX = 70;   // how far sideways a fall may drift

        // Edge costs. Walking is cheap, jumping is slow and can fail, falling is fast.
        int   costWalk = 10;
        int   costJump = 90;
        int   costDrop = 25;
        int   costRope = 70;
        int   ropeSnapTol = 90;

        bool  useWallCheck = true;
        int   targetCostCandidates = 5;

        // Edge reliability: a jump/down-jump that doesn't land on the
        // foothold it was aimed at counts as a failed attempt. Enough
        // consecutive fails on the same edge hard-blacklists it right away
        // instead of waiting on stuckGiveupMs/routeGiveupMs; failCount also
        // soft-penalizes that edge's A* cost so a shaky-but-not-yet-banned
        // edge gets deprioritized in favor of a more reliable route.
        int   edgeFailThreshold = 2;
        int   edgeFailPenalty = 150;

        // ---------- Movement -----------------------------------------------------------
        int   arriveTol = 10;   // within this of the waypoint -> stand still
        int   dirHysteresis = 26;   // must exceed this to reverse direction
        int   actionMargin = 26;   // how close to takeoffX before jumping
        int   walkOvershoot = 30;   // walk this far past a seam to cross it
        int   aimTol = 10;   // ignore landing offsets smaller than this
        int   jumpCooldownMs = 200;
        int   airLockMs = 300;  // keep the jump direction this long
        bool  useDownjump = true;
        int   downjumpVy = 220;
        int   boostBrakeGap = 70;

        // ---------- Timing -------------------------------------------------------------
        DWORD pathCacheMs = 700;   // reuse a computed route this long
        DWORD stuckMs = 1200;  // no progress this long = stuck
        DWORD stuckGiveupMs = 4000;  // stuck this long = abandon the drop
        int   pathMaxNodes = 512;

        bool  requireTelescope = true;
        bool  debugPathfind = true;
        bool  dumpMapGraph = false;
        DWORD debugIntervalMs = 1000;
        DWORD routeGiveupMs = 9000;
        double petSpeedMult = 1.8;
    };

    extern PetHelperConfig g_config;
    extern bool g_forceMapRebuild;
    extern volatile bool g_isMapTransitioning;

    // ===========================================================================
    //  Types
    // ===========================================================================
    struct TargetDrop { int x; int y; int dist; };
    struct FhNode { void* fh; int x1, y1, x2, y2; };
    struct LadderNode { int x, yTop, yBottom; bool isLadder; };

    struct DropBlacklistEntry { int x, y; DWORD expire; };
    struct BlacklistEntry { int ia, ib; DWORD expire; };

    // Sentinel expire value: never times out (cleared explicitly on map
    // change instead - see Hooked_CField_Init). Used for drops confirmed to
    // be off any platform, which is a geometric fact that won't change.
    static const DWORD DROP_BLACKLIST_FOREVER = 0xFFFFFFFF;

    enum EdgeType { EDGE_WALK = 0, EDGE_JUMP = 1, EDGE_DROP = 2, EDGE_ROPE = 3 };

    struct Edge {
        int to;
        int type;
        int takeoffX;   // where on the source foothold we leave from
        int landingX;   // expected landing X on the destination
        int cost;
        int failCount = 0;  // consecutive-attempt reliability penalty, see edgeFailPenalty
    };

    struct RouteStep {
        void* fh;
        Edge  edge;
        bool  hasEdge;
    };

    struct PetRoute {
        std::vector<RouteStep> steps;
        void* startFh;
        int   tgtX, tgtY;
        DWORD time;
        bool  valid;
    };

    // Encapsulated state per pet slot (0..2)
    struct PetSlotContext {
        void* pCPet = NULL;
        void* pVecCtrl = NULL;
        PetRoute route;
        int   lastDir = 0;
        int   wantGap = 0;
        bool  hasTelescope = false;
        bool  slotPrimed = false;
        bool  boosting = false;

        int   targetX = -1;
        int   targetY = -1;
        DWORD lockSeen = 0;

        int   onTopTargetX = -1;
        int   onTopTargetY = -1;
        DWORD onTopSince = 0;

        DWORD lastMoveT = 0;
        DWORD noDropSince = 0;
        DWORD petPauseUntil = 0;

        int   lastTgtX = -99999;
        int   lastTgtY = -99999;
        int   bestDist = 999999;

        DWORD lastJumpTime = 0;
        int   lastJumpDir = 0;

        // In-flight jump/down-jump edge attempt, resolved once the pet is
        // grounded again (see MyPetAI_Update_Inner). Landing anywhere other
        // than pendingEdgeToIdx after leaving pendingEdgeFromFh counts as a
        // failed attempt and feeds edgeFailStreak.
        void* pendingEdgeFromFh = NULL;
        int   pendingEdgeToIdx = -1;
        DWORD pendingEdgeSince = 0;
        int   edgeFailStreak = 0;

        // Rope climbing animation state
        bool  isClimbingRope = false;
        int   ropeX = 0;        // rope's true X, held fixed for the whole climb (see ropeSnapTol)
        int   ropeTargetY = 0;
        int   ropeStartY = 0;
        void* ropeTargetFh = NULL;

        void Reset() {
            pCPet = NULL;
            pVecCtrl = NULL;
            route.valid = false;
            route.steps.clear();
            lastDir = 0;
            wantGap = 0;
            hasTelescope = false;
            slotPrimed = false;
            boosting = false;

            isClimbingRope = false;
            ropeX = 0;
            ropeTargetY = 0;
            ropeStartY = 0;
            ropeTargetFh = NULL;

            targetX = -1;
            targetY = -1;
            lockSeen = 0;

            onTopTargetX = -1;
            onTopTargetY = -1;
            onTopSince = 0;

            lastMoveT = 0;
            noDropSince = 0;
            petPauseUntil = 0;

            lastTgtX = -99999;
            lastTgtY = -99999;
            bestDist = 999999;

            lastJumpTime = 0;
            lastJumpDir = 0;

            pendingEdgeFromFh = NULL;
            pendingEdgeToIdx = -1;
            pendingEdgeSince = 0;
            edgeFailStreak = 0;
        }
    };

    extern PetSlotContext g_pets[3];

    // Backward-compatibility references for existing code
    extern bool (&cachedHasTelescope)[3];
    extern PetRoute (&g_route)[3];
    extern int (&g_lastDir)[3];

    // ===========================================================================
    //  TargetManager - drop scanning + per-pet target lock/blacklist
    // ===========================================================================
    class TargetManager {
    public:
        static int currentPetIdx;
        static int poolScanned;

        static std::vector<DropBlacklistEntry> dropBlacklist;

        static void Init();
        // durationMs = 0 uses g_config.dropBlacklistMs (the generic case).
        static void BlacklistDrop(int x, int y, DWORD now, DWORD durationMs = 0);
        static bool IsBlacklisted(int x, int y);
        static bool CheckUnpickable(int petIdx, int petX, int petY,
            int tx, int ty, DWORD now, bool logNow = false);
        static void LockTarget(int petIdx, int tx, int ty, DWORD now);
        static void ClearTarget(int petIdx);
        static void ExpireLock(int petIdx, DWORD now);

        static std::vector<TargetDrop> CollectSortedDrops(int fromX, int fromY,
            bool logNow, DWORD now);
    };

    // ===========================================================================
    //  Pathfinder - adjacency graph + A*
    // ===========================================================================
    class Pathfinder {
    public:
        static std::vector<FhNode>            fh;
        static std::vector<std::vector<Edge> > adj;
        static std::vector<char>              ledgeL, ledgeR;
        static DWORD                          fhTime;
        static void* fhSpace;
        static std::vector<BlacklistEntry>    blacklist;
        // Ladders/ropes read this map refresh, kept only for DUMP_MAP_GRAPH /
        // debugging - BuildAdjacency()/AddRopeEdges() already folded them
        // into adj[] as EDGE_ROPE edges.
        static std::vector<LadderNode>        ropes;

        // A* working buffers
        static std::vector<int>  s_gScore;
        static std::vector<int>  s_parent;
        static std::vector<int>  s_parentEdge;
        static std::vector<char> s_closed;

        // ---- geometry helpers (kept inline: called from the O(n^2) graph
        // build and from A*'s inner loop, so out-of-lining them would show up
        // as real overhead on maps with a lot of footholds). ------------------
        static int LoX(const FhNode& n) { return (n.x1 < n.x2) ? n.x1 : n.x2; }
        static int HiX(const FhNode& n) { return (n.x1 < n.x2) ? n.x2 : n.x1; }
        static int MidX(const FhNode& n) { return (n.x1 + n.x2) / 2; }
        static int MidY(const FhNode& n) { return (n.y1 + n.y2) / 2; }

        static int YAtX(const FhNode& n, int x) {
            if (n.x2 == n.x1) return n.y1;
            return n.y1 + (x - n.x1) * (n.y2 - n.y1) / (n.x2 - n.x1);
        }

        static int Clamp(int v, int lo, int hi) {
            if (v < lo) return lo;
            if (v > hi) return hi;
            return v;
        }

        static void LeftPt(const FhNode& n, int& x, int& y) {
            if (n.x1 <= n.x2) { x = n.x1; y = n.y1; }
            else { x = n.x2; y = n.y2; }
        }
        static void RightPt(const FhNode& n, int& x, int& y) {
            if (n.x1 <= n.x2) { x = n.x2; y = n.y2; }
            else { x = n.x1; y = n.y1; }
        }

        static int Heuristic(int ia, int ib) {
            int dx = abs(MidX(fh[ia]) - MidX(fh[ib]));
            int dy = abs(MidY(fh[ia]) - MidY(fh[ib]));
            // Divide by 8 (was 12): tighter admissible heuristic, guides A*
            // more directly on complex multi-platform maps.
            return (dx + dy * 2) / 8;
        }

        // ---- everything below is defined in PetHelper_Pathfinder.cpp -----------
        static bool LoadFhNode(void* pFh, FhNode& out);
        static void EnumFootholds(std::vector<FhNode>& out);
        static bool AnyEndpointNear(int idx, int x, int y);
        static int  FindFhIndex(void* pFh);
        static bool IsLinkBlacklisted(int ia, int ib);
        static void BlacklistLink(void* pFhA, void* pFhB, DWORD now);
        // Adjusts adj[fromIdx]'s edge(s) landing on toIdx: success decays
        // failCount by 1 (floor 0), failure raises it by 1. Called once a
        // pending jump/down-jump attempt is resolved.
        static void RecordEdgeOutcome(int fromIdx, int toIdx, bool success);
        static bool MakeEdge(int ia, int ib, Edge& out);
        static void BuildAdjacency();
        static void EnumLadders(std::vector<LadderNode>& out);
        static int  FindFootholdSpanning(int x, int y, int tol);
        static void AddRopeEdges();
        static void RefreshFhCache(DWORD now);

        struct PQNode {
            int idx;
            int f;
            bool operator>(const PQNode& o) const { return f > o.f; }
        };

        // Produces a full route: each step knows the edge used to reach the next.
        static bool FindRoute(void* startFh, void* goalFh, std::vector<RouteStep>& out);

        // Sum of edge costs along a route already found by FindRoute (0 for a
        // same-foothold route, INT_MAX if unreachable). Used by target
        // selection to rank candidate drops by real traversal cost instead of
        // straight-line distance.
        static int RouteCost(const std::vector<RouteStep>& steps);
    };

    // ===========================================================================
    //  PetController - executes the route
    // ===========================================================================
    class PetController {
    public:
        static DWORD lastJumpTime[3];
        static int   lastJumpDir[3];

        static void DownJump(void* pThis, int petX, int petY, int dir);

        // kind: 0 none, 1 normal jump, 2 down-jump
        static void ApplyMove(void* pThis, int petIdx, int dir, int kind,
            int petX, int petY);

        // Turn a desired X into a direction, with a dead zone and hysteresis so the
        // pet stops instead of flickering when it is already close enough.
        static int SteerTo(int petIdx, int petX, int wantX);
    };

    // ---------- Route cache helpers (PetHelper_Controller.cpp) ---------------------
    bool EnsureRoute(int petIdx, void* startFh, void* goalFh,
        int tx, int ty, DWORD now, int& outIndex);

    // ---------- Shared utility (PetHelper_SEH.cpp) ----------------------------------
    void PatchMemory(DWORD addr, const std::vector<BYTE>& bytes);

    // ---------- Hook installers defined in PetHelper_MiscHooks.cpp -----------------
    void WriteHook();
    void WriteUpdatePetAbilityHook();
    void WriteStringPoolHook();

} // namespace PetHelper
