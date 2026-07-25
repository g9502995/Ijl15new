#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <oaidl.h>
#include <oleauto.h>
#include <unknwn.h>

// --- WZ COM Interfaces ---
struct IWzProperty : public IUnknown {
    virtual HRESULT __stdcall get_persistentUOL(wchar_t**) = 0;
    virtual HRESULT __stdcall raw_Serialize(void*) = 0;
    virtual HRESULT __stdcall get_item(const wchar_t*, tagVARIANT*) = 0;
    virtual HRESULT __stdcall put_item(const wchar_t*, tagVARIANT) = 0;
    virtual HRESULT __stdcall get__NewEnum(IUnknown**) = 0;
    virtual HRESULT __stdcall get_count(unsigned int*) = 0;
};

struct IWzResMan : public IUnknown {
    virtual HRESULT __stdcall get_persistentUOL(wchar_t**) = 0;
    virtual HRESULT __stdcall raw_Serialize(void*) = 0;
    virtual HRESULT __stdcall unknown_method1() = 0;
    virtual HRESULT __stdcall unknown_method2() = 0;
    virtual HRESULT __stdcall raw_GetObject(const wchar_t*, tagVARIANT, tagVARIANT, tagVARIANT*) = 0;
};

class Ztl_variant_t : public tagVARIANT {
public:
    inline Ztl_variant_t() { VariantInit(this); }
    inline ~Ztl_variant_t() { VariantClear(this); }
    inline IUnknown* GetUnknown(bool fAddRef, bool fTryChangeType) {
        if (this->vt == VT_UNKNOWN || this->vt == VT_DISPATCH) {
            if (fAddRef && this->punkVal) this->punkVal->AddRef();
            return this->punkVal;
        }
        return nullptr;
    }
};

// --- Parsed Set Data Structures ---
struct SetEffect {
    int count;
    std::vector<std::pair<std::string, int>> stats; // e.g. {"incSTR", 10}
};

struct SetInfo {
    int setID;
    std::wstring setName;
    int completeCount;
    std::vector<int> itemIDs;
    std::vector<SetEffect> effects;
};

extern std::map<int, SetInfo> g_SetInfos;
extern std::map<int, int> g_ItemToSetID;
extern std::mutex g_SetItemMutex;

void LoadSetItemInfo();

// Set panel data structures
struct SetPanelItemInfo {
    int id;
    std::string name;
    std::string category;
    bool isEquipped;
};

struct SetPanelEffectInfo {
    int count;
    bool isActive;
    std::vector<std::pair<std::string, int>> stats;
};

struct SetPanelData {
    bool active;
    std::string setName;
    int equippedCount;
    int totalCount;
    std::vector<SetPanelItemInfo> items;
    std::vector<SetPanelEffectInfo> effects;
    DWORD lastUpdated;
    int hideFrameCount;
    int nativeX;
    int nativeY;
    int nativeWidth;
    int nativeHeight;
    int anchorY;
    void* pToolTip;
    void* pEquip;
    float drawX;
    float drawY;
    float startMouseX;
    float startMouseY;
};

// Called from TooltipHook when hovering equipment
void UpdateSetPanelData(void* pToolTip, void* pEquip);
void ClearSetPanelData(void* pToolTip);

// Called from D3D EndScene every frame
void DrawSetItemImGui();

extern std::mutex g_SetPanelMutex;
extern SetPanelData g_SetPanelData;
