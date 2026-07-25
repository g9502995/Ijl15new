#pragma once

void GUI_Init();
void GUI_Render();
void GUI_Reset();
bool GUI_WndProc(void* hWnd, unsigned int msg, unsigned int wParam, long lParam);
