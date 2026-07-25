#pragma once
#include <windows.h>

void SetEquipCustomValue(void* pEquip, unsigned __int64 value);
unsigned __int64 GetEquipCustomValue(void* pEquip);
void RemoveEquipCustomValue(void* pEquip);
void SetCurrentEquipRawPos(short rawPos);
short GetCurrentEquipRawPos();
void SetCustomEquipCount(short rawPos, int count);
int GetCustomEquipCount(short rawPos, bool* hasValue);
void InitEquipToolTipHook();
void InitCustomEquipSystem();
