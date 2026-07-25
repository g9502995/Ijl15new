#include "stdafx.h"
#include "PetHelper_Internal.h"

// ===========================================================================
//  Raw memory-read primitives shared by every PetHelper translation unit.
//  Everything here is a thin __try/__except wrapper around a game pointer -
//  the game itself may hand us a stale/NULL pointer during a map transition,
//  and these guards are what keeps that from crashing the client.
// ===========================================================================

// Position getter shared by CUser / CPet (game pattern at 0x9C44B7, 0x9C485F):
//   mov eax,[esi] / mov ecx,esi / call [eax+10h]   ; esi = obj+4
BOOL SEH_GetObjPos(void* obj, int* pOutX, int* pOutY)
{
    BOOL ok = FALSE;
    __try {
        void* pSlot = (void*)((char*)obj + 4);
        DWORD* vtbl = *(DWORD**)pSlot;
        if (!vtbl) __leave;
        PFN_GetPos fn = (PFN_GetPos)(vtbl[4]);
        if (!fn) __leave;
        void* pRes = fn(pSlot);
        if (!pRes) __leave;
        *pOutX = *(int*)pRes;
        *pOutY = *(int*)((char*)pRes + 4);
        ok = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

BOOL SEH_ReadPetAbility(void* pPet, int* out)
{
    BOOL ok = FALSE;
    __try {
        const long* key = (const long*)((char*)pPet + PET_ABIL_KEY);
        unsigned int enc = *(unsigned int*)((char*)pPet + PET_ABIL_ENC);
        *out = (int)g_ZtlSecureFuse(key, enc);
        ok = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

// nType: 3 = normal item, other values are meso / special drops.
BOOL SEH_GetDropInfo(void* pDropObj, int* pOutX, int* pOutY, int* pOutType)
{
    BOOL ok = FALSE;
    __try {
        char* d = (char*)pDropObj;
        if (*(DWORD*)(d + 0x88) == 0) __leave;
        if (*(BYTE*)(d + 0x1D) == 0) __leave;
        *pOutType = *(DWORD*)(d + 0x48);
        PFN_GetDataLong pfn = (PFN_GetDataLong)0x0042873D;
        *pOutX = (int)pfn((const void*)(d + 0x74));
        *pOutY = (int)pfn((const void*)(d + 0x68));
        ok = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

void* SEH_GetFootholdClosest(int x, int y)
{
    void* pSpace = *g_ppPhysicalSpace;
    if (!pSpace) return NULL;
    void* fh = NULL;
    __try { fh = g_GetFootholdClosest(pSpace, x, y); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return fh;
}

void* SEH_GetFootholdUnder(int x, int y, int* pcy)
{
    void* pSpace = *g_ppPhysicalSpace;
    if (!pSpace) return NULL;
    void* fh = NULL;
    int   cy = INT_MAX;
    __try { fh = g_GetFootholdUnderneath(pSpace, x, y, &cy, INT_MAX, 10); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    if (pcy) *pcy = cy;
    return fh;
}

BOOL SEH_ReadFoothold(void* fh, int* x1, int* y1, int* x2, int* y2, int* attr)
{
    BOOL ok = FALSE;
    __try {
        *x1 = FH_X1(fh); *y1 = FH_Y1(fh);
        *x2 = FH_X2(fh); *y2 = FH_Y2(fh);
        *attr = FH_ATTR(fh);
        ok = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

// Field offsets confirmed from CWvsPhysicalSpace2D::Load's ladderRope parse
// loop (0xA43E7B): id@0x00, l@0x04, uf@0x08, x@0x0C, y1@0x10, y2@0x14, page@0x18.
BOOL SEH_ReadLadderEntry(void* pEntry, int* x, int* yTop, int* yBottom, int* isLadder)
{
    BOOL ok = FALSE;
    __try {
        *isLadder = *(int*)((char*)pEntry + 0x04);
        *x        = *(int*)((char*)pEntry + 0x0C);
        *yTop     = *(int*)((char*)pEntry + 0x10);
        *yBottom  = *(int*)((char*)pEntry + 0x14);
        ok = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

// See PetHelper_Internal.h - fails open (TRUE = unobstructed) so a bad read
// or an unexpected engine state can only widen the graph back to the old
// endpoint-proximity behavior, never make every edge look wall-blocked.
BOOL SEH_CanGoThrough(int x1, int y1, int x2, int y2)
{
    void* pSpace = *g_ppPhysicalSpace;
    if (!pSpace) return TRUE;
    BOOL ok = TRUE;
    __try {
        int ox = x2, oy = y2;
        ok = (g_CanGoThrough(pSpace, x1, y1, &ox, &oy, 0) != 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = TRUE; }
    return ok;
}

BOOL SEH_ReadPtr(void* p, void** out)
{
    BOOL ok = FALSE;
    __try { *out = *(void**)p; ok = TRUE; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

BOOL SEH_ReadInt(void* p, int* out)
{
    BOOL ok = FALSE;
    __try { *out = *(int*)p; ok = TRUE; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

// A foothold pointer must be readable and describe a sane segment. The owner's
// VecCtrl field can hold small integers during map transitions, and passing
// those to SetActive dereferences them at +0x1C and crashes.
BOOL SEH_IsValidFoothold(void* fh)
{
    if ((DWORD)fh < 0x10000) return FALSE;
    BOOL ok = FALSE;
    __try {
        int x1 = *(int*)((char*)fh + 0x0C);
        int x2 = *(int*)((char*)fh + 0x14);
        int y1 = *(int*)((char*)fh + 0x10);
        volatile int probe = *(int*)((char*)fh + 0x1C);
        (void)probe;
        ok = (x1 != x2) && (abs(x1) < 1000000) && (abs(y1) < 1000000);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

BOOL SEH_ScalePhysics(double* t, double mult, double* outSpeed, double* outForce)
{
    BOOL ok = FALSE;
    __try {
        *outSpeed = t[1];
        *outForce = t[2];
        t[1] = *outSpeed * mult;
        t[2] = *outForce * mult;
        ok = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

void SEH_RestorePhysics(double* t, double speed, double force)
{
    __try { t[1] = speed; t[2] = force; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

namespace PetHelper {

    void PatchMemory(DWORD addr, const std::vector<BYTE>& bytes)
    {
        DWORD old;
        if (VirtualProtect((LPVOID)addr, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) {
            memcpy((void*)addr, bytes.data(), bytes.size());
            VirtualProtect((LPVOID)addr, bytes.size(), old, &old);
        }
    }

} // namespace PetHelper
