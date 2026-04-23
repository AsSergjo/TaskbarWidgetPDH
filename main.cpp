#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <pdh.h>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <atomic>
#include <vector>
#include <algorithm>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")

#include "resource.h"

//IDs
#define IDM_EXIT              101
#define WM_APP_UPDATE_METRICS (WM_APP + 1)

// Window geometry
static const int WIN_W = 166;
static const int WIN_H = 62;

// Side buttons (mode selector)
static const int SQBUTTON_W = 14;
static const int SQBUTTON_PAD = 4;

// Visual style
// Overall window alpha (0-255).  220 ~= 86 % — feels solid but clearly see-through.
static const BYTE WIN_ALPHA = 220;

// Background colour painted in WM_PAINT (layered alpha makes it semi-transparent)
// Muted Gray
static const COLORREF CLR_BG = RGB(60, 62, 66);
static const COLORREF CLR_BORDER = RGB(100, 105, 115);
static const COLORREF CLR_DN = RGB(120, 230, 255);
static const COLORREF CLR_UP = RGB(255, 200, 80);
static const COLORREF CLR_VAL = RGB(255, 255, 255);
static const COLORREF CLR_CPU = RGB(120, 230, 255);
static const COLORREF CLR_DSK = RGB(120, 255, 120);
static const COLORREF CLR_MEM_A = RGB(120, 230, 255);
static const COLORREF CLR_MEM_U = RGB(120, 255, 120);

// Registry key
static const wchar_t* REG_KEY = L"Software\\NetMonitorWidget";

//Globals
HINSTANCE        g_hInst        = NULL;
HWND             g_hPopup       = NULL;
HANDLE           g_hMutex       = NULL;
CRITICAL_SECTION g_cs;

enum class DisplayMode { Network, CpuDisk, Memory };
DisplayMode g_currentMode = DisplayMode::Network;

// Fonts: created once in WM_CREATE, deleted in WM_DESTROY

HFONT g_hFontValue = NULL;   // monospaced value font
HFONT g_hFontFluent = NULL;  // Segoe Fluent Icons для CPU/Disk/Mem

PDH_HQUERY   hQuery   = nullptr;
PDH_HCOUNTER hNetDown = nullptr, hNetUp = nullptr;
PDH_HCOUNTER hCpu = nullptr, hDiskTime = nullptr;
PDH_HCOUNTER hMemAvail = nullptr, hMemUsed = nullptr;

std::atomic<bool> g_keepRunning{true};
HANDLE            g_updateThread = nullptr;

// Two separate strings so we can colour them independently in WM_PAINT
struct NetLine { std::wstring arrow; std::wstring value; };
NetLine g_lineDown, g_lineUp;
NetLine g_lineCpu, g_lineDisk;
NetLine g_lineMemAvail, g_lineMemUsed;

// Drag state
bool  g_isDragging = false;
POINT g_dragOffset = {};

LRESULT CALLBACK PopupWndProc(HWND, UINT, WPARAM, LPARAM);

//Registry helpers
static void SavePosition(int x, int y) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        DWORD dx = (DWORD)x, dy = (DWORD)y;
        RegSetValueExW(hk, L"X", 0, REG_DWORD, (BYTE*)&dx, sizeof(dx));
        RegSetValueExW(hk, L"Y", 0, REG_DWORD, (BYTE*)&dy, sizeof(dy));
        RegCloseKey(hk);
    }
}

static bool LoadPosition(int& x, int& y) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0,
                      KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return false;
    DWORD dx = 0, dy = 0, sz = sizeof(DWORD);
    bool ok =
        RegQueryValueExW(hk, L"X", 0, nullptr, (BYTE*)&dx, &sz) == ERROR_SUCCESS &&
        RegQueryValueExW(hk, L"Y", 0, nullptr, (BYTE*)&dy, &sz) == ERROR_SUCCESS;
    RegCloseKey(hk);
    if (ok) { x = (int)dx; y = (int)dy; }
    return ok;
}

//DWM rounded corners (Windows 11+)
static void ApplyRoundedCorners(HWND hwnd) {
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (!hDwm) return;
    typedef HRESULT(WINAPI* PFN)(HWND, DWORD, LPCVOID, DWORD);
    PFN fn = (PFN)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    if (fn) {
        DWORD pref = 2; // DWMWCP_ROUND
        fn(hwnd, 33, &pref, sizeof(pref));
    }
    FreeLibrary(hDwm);
}

//PDH
static PDH_STATUS InitPDH() {
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &hQuery);
    if (st != ERROR_SUCCESS) return st;

    PdhAddEnglishCounterW(hQuery,
        L"\\Network Interface(*)\\Bytes Received/sec", 0, &hNetDown);
    PdhAddEnglishCounterW(hQuery,
        L"\\Network Interface(*)\\Bytes Sent/sec",     0, &hNetUp);
    PdhAddEnglishCounterW(hQuery,
        L"\\Processor Information(_Total)\\% Processor Time", 0, &hCpu);
    PdhAddEnglishCounterW(hQuery,
        L"\\PhysicalDisk(_Total)\\% Disk Time", 0, &hDiskTime);
    PdhAddEnglishCounterW(hQuery,
        L"\\Memory\\Available MBytes", 0, &hMemAvail);
    PdhAddEnglishCounterW(hQuery,
        L"\\Memory\\% Committed Bytes In Use", 0, &hMemUsed);

    PdhCollectQueryData(hQuery);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    PdhCollectQueryData(hQuery);
    return ERROR_SUCCESS;
}

static double SumCounterArray(PDH_HCOUNTER hCtr) {
    if (!hCtr) return 0.0;
    DWORD sz = 0, cnt = 0;
    PDH_STATUS st = PdhGetFormattedCounterArray(
        hCtr, PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
    if (st != (PDH_STATUS)0x800007D2L && st != ERROR_SUCCESS) return 0.0;
    std::vector<BYTE> buf(sz);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArray(hCtr, PDH_FMT_DOUBLE,
                                    &sz, &cnt, items) != ERROR_SUCCESS)
        return 0.0;
    double total = 0.0;
    for (DWORD i = 0; i < cnt; ++i) {
        double v = items[i].FmtValue.doubleValue;
        if (v >= 0.0 && !std::isnan(v) && !std::isinf(v)) total += v;
    }
    return total;
}

static double GetCounterValue(PDH_HCOUNTER hCtr) {
    if (!hCtr) return 0.0;
    PDH_FMT_COUNTERVALUE val;
    if (PdhGetFormattedCounterValue(hCtr, PDH_FMT_DOUBLE,
                                    nullptr, &val) != ERROR_SUCCESS) return 0.0;
    double v = val.doubleValue;
    return (v >= 0.0 && !std::isnan(v) && !std::isinf(v)) ? v : 0.0;
}

static void FetchMetrics() {
    PdhCollectQueryData(hQuery);
    double dn = SumCounterArray(hNetDown) / 1024.0;
    double up = SumCounterArray(hNetUp)   / 1024.0;
    wchar_t du = L'K'; if (dn >= 1024.0) { dn /= 1024.0; du = L'M'; }
    wchar_t uu = L'K'; if (up >= 1024.0) { up /= 1024.0; uu = L'M'; }

    wchar_t vd[64], vu[64];
    swprintf_s(vd, L" %5.1f %cB/s", dn, du);
    swprintf_s(vu, L" %5.1f %cB/s", up, uu);

    double cpu = GetCounterValue(hCpu);
    double dsk = GetCounterValue(hDiskTime);
    wchar_t vc[64], vk[64];
    swprintf_s(vc, L" %5.1f %%", cpu);
    swprintf_s(vk, L" %5.1f %%", dsk);

    double memAvail = GetCounterValue(hMemAvail);
    double memUsed  = GetCounterValue(hMemUsed);
    wchar_t vma[64], vmu[64];
    if (memAvail >= 1024.0) {
        swprintf_s(vma, L" %5.1f GB", memAvail / 1024.0);
    } else {
        swprintf_s(vma, L" %5.0f MB", memAvail);
    }
    swprintf_s(vmu, L" %5.1f %%", memUsed);
 
    EnterCriticalSection(&g_cs);
	
    g_lineDown =        {L"\uf08e", vd};   // down arrow
    g_lineUp   =          {L"\uf090", vu};   // up arrow
    g_lineCpu   =        {L"\uEEA1", vc};   // CPU       - % Processor Time
    g_lineDisk  =         {L"\uEDA2", vk};   // HardDrive - % Disk Time
    g_lineMemAvail =  {L"\uEEA0", vma};   // RAM       - Available MBytes
    g_lineMemUsed  = {L"\uF164", vmu};   // custom    - % Committed Bytes In Use
	
    LeaveCriticalSection(&g_cs);
}

//Background thread
DWORD WINAPI UpdateThread(LPVOID) {
    while (g_keepRunning) {
        FetchMetrics();
        if (g_hPopup) PostMessageW(g_hPopup, WM_APP_UPDATE_METRICS, 0, 0);
        Sleep(1000);
    }
    return 0;
}

//Painting
static void PaintWidget(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;

    // Double-buffer setup
    HDC     memDC  = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
    // Background
    HBRUSH hBgBrush = CreateSolidBrush(CLR_BG);
    FillRect(memDC, &rc, hBgBrush);
    DeleteObject(hBgBrush);
    // 1-px border
    HBRUSH hBdBrush = CreateSolidBrush(CLR_BORDER);
    FrameRect(memDC, &rc, hBdBrush);
    DeleteObject(hBdBrush);

    SetBkMode(memDC, TRANSPARENT);

    // Three mode-selector squares on the right
    const int sqW = 14, pad = 4;
    const int sqX = rc.right - sqW - pad - 2;
    RECT sq[3];
    for (int i = 0; i < 3; ++i) {
        sq[i] = {sqX, pad + i * (sqW + pad), sqX + sqW, pad + (i + 1) * sqW + i * pad};
    }

    for (int i = 0; i < 3; ++i) {
        bool isCurrent = (int)g_currentMode == i;
        COLORREF c = isCurrent ? CLR_VAL : CLR_BORDER;
        SelectObject(memDC, g_hFontFluent);
        SetTextColor(memDC, c);
        DrawTextW(memDC, L"\ue915", -1, &sq[i],
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // Text
    EnterCriticalSection(&g_cs);
    NetLine line1 = g_lineDown, line2 = g_lineUp;
    if (g_currentMode == DisplayMode::CpuDisk) { line1 = g_lineCpu; line2 = g_lineDisk; }
    if (g_currentMode == DisplayMode::Memory)  { line1 = g_lineMemAvail; line2 = g_lineMemUsed; }
    LeaveCriticalSection(&g_cs);
    // Measure line height
    HFONT hOld = (HFONT)SelectObject(memDC, g_hFontValue);
    TEXTMETRICW tm; GetTextMetricsW(memDC, &tm);
    int lh = tm.tmHeight + tm.tmExternalLeading + 2;

    int totalH = lh * 2;
    int yBase  = (rc.bottom - totalH) / 2;

    const int xPad  = 10;
    const int arWid = 30; // wider for "CPU", "Disk"

    auto DrawLine = [&](const NetLine& line, int y, COLORREF arrowClr) {
          
        SelectObject(memDC, g_hFontFluent);
        SetTextColor(memDC, arrowClr);
        RECT rA = {xPad, y, xPad + arWid, y + lh};
        DrawTextW(memDC, line.arrow.c_str(), -1, &rA,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(memDC, g_hFontValue);
        SetTextColor(memDC, CLR_VAL);
        // Shrink value rect to not overlap squares
        RECT rV = {xPad + arWid, y, rc.right - xPad - sqW - pad, y + lh};
        DrawTextW(memDC, line.value.c_str(), -1, &rV,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };

    COLORREF clr1 = CLR_DN, clr2 = CLR_UP;
    if (g_currentMode == DisplayMode::CpuDisk) { clr1 = CLR_CPU; clr2 = CLR_DSK; }
    if (g_currentMode == DisplayMode::Memory)  { clr1 = CLR_MEM_A; clr2 = CLR_MEM_U; }

    DrawLine(line1, yBase,      clr1);
    DrawLine(line2, yBase + lh, clr2);

    SelectObject(memDC, hOld);

    // Flush the completed frame to the screen.
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    // Clean up off-screen resources.
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(hwnd, &ps);
}

// Window procedure
LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE:
        g_hFontValue = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
            L"Consolas");
			
		g_hFontFluent = CreateFontW(
           -14, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe Fluent Icons");

        // Overall window transparency
        SetLayeredWindowAttributes(hwnd, 0, WIN_ALPHA, LWA_ALPHA);

        // DWM rounded corners (Windows 11+, silently ignored on older versions)
        ApplyRoundedCorners(hwnd);
        return 0;

    case WM_PAINT:
        PaintWidget(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_APP_UPDATE_METRICS:
        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);
        return 0;

    // All hit-testing returns HTCLIENT so we handle every mouse message
    case WM_NCHITTEST:
        return HTCLIENT;

    // Drag
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        // Check if click is on one of the side buttons
        RECT rc; GetClientRect(hwnd, &rc);
        const int sqW = 18, pad = 4;
        const int sqX = rc.right - sqW - pad;
        RECT sq[3];
        for (int i = 0; i < 3; ++i) {
            sq[i] = {sqX, pad + i * (sqW + pad), sqX + sqW, pad + (i + 1) * sqW + i * pad};
            if (PtInRect(&sq[i], pt)) {
                g_currentMode = (DisplayMode)i;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0; // Handled: don't start drag
            }
        }

        // If not on a button, start dragging
        if (!g_isDragging) {
            g_isDragging = true;
            SetCapture(hwnd);
            ClientToScreen(hwnd, &pt);
            RECT wr; GetWindowRect(hwnd, &wr);
            g_dragOffset = {pt.x - wr.left, pt.y - wr.top};
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_isDragging) {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &pt);
            int nx = pt.x - g_dragOffset.x;
            int ny = pt.y - g_dragOffset.y;
            // Clamp to virtual desktop (multi-monitor aware)
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            nx = std::max(vx, std::min(nx, vx + vw - WIN_W));
            ny = std::max(vy, std::min(ny, vy + vh - WIN_H));
            SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_isDragging) {
            g_isDragging = false;
            ReleaseCapture();
            RECT wr; GetWindowRect(hwnd, &wr);
            SavePosition(wr.left, wr.top);
        }
        return 0;

    // After a window-move (e.g. via NCLBUTTONDOWN trick), restore TOPMOST
    case WM_EXITSIZEMOVE:
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;

    // Double-click -> Task Manager
    case WM_LBUTTONDBLCLK: {
        g_isDragging = false;
        ReleaseCapture();
        
        INPUT inputs[6] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_CONTROL;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_SHIFT;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wVk = VK_ESCAPE;
        
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wVk = VK_ESCAPE;
        inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[4].type = INPUT_KEYBOARD;
        inputs[4].ki.wVk = VK_SHIFT;
        inputs[4].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[5].type = INPUT_KEYBOARD;
        inputs[5].ki.wVk = VK_CONTROL;
        inputs[5].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
        return 0;
    }

    // Context menu
    case WM_CONTEXTMENU: {
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Закрыть");
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hMenu);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_EXIT)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        break;

    // Cleanup
    case WM_DESTROY:
        g_keepRunning = false;
        if (g_updateThread) {
            WaitForSingleObject(g_updateThread, 2000);
            CloseHandle(g_updateThread);
            g_updateThread = nullptr;
        }
        if (hQuery)        { PdhCloseQuery(hQuery); hQuery = nullptr; }
        if (g_hFontValue)  { DeleteObject(g_hFontValue); g_hFontValue = nullptr; }
		if (g_hFontFluent) { DeleteObject(g_hFontFluent); g_hFontFluent = nullptr; }
        if (g_hMutex)      { ReleaseMutex(g_hMutex); CloseHandle(g_hMutex); g_hMutex = nullptr; }
        DeleteCriticalSection(&g_cs);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

//WinMain
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_hMutex = CreateMutexW(NULL, TRUE, L"NetMonitorWidget_Mutex_v3");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_hMutex);
        return 0;
    }

    g_hInst = hInst;
    InitializeCriticalSection(&g_cs);
    // Placeholder text while PDH warms up
    EnterCriticalSection(&g_cs);
    g_lineDown = {L"\uf08e", L" ---.- KB/s"};
    g_lineUp   = {L"\uf090", L" ---.- KB/s"};
    g_lineCpu      = {L"\uEE77", L"  ---.- %"};
    g_lineDisk     = {L"\uE950", L"  ---.- %"};
    g_lineMemAvail = {L"\uE7F1", L" ---.- GB"};
    g_lineMemUsed  = {L"\uEE6D", L"  ---.- %"};
    LeaveCriticalSection(&g_cs);

    PDH_STATUS pdhStatus = InitPDH();
    if (pdhStatus != ERROR_SUCCESS) {
        wchar_t msg[256];
        swprintf_s(msg, L"Не удалось инициализировать счётчики производительности (PDH).\n"
                        L"Возможно, они повреждены.\n\n"
                        L"Код ошибки: 0x%08X\n\n"
                        L"Попробуйте выполнить 'lodctr /R' в командной строке от имени администратора.",
                   pdhStatus);
        MessageBoxW(NULL, msg, L"Ошибка NetMonitorWidget", MB_OK | MB_ICONERROR);
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
        DeleteCriticalSection(&g_cs);
        return 1;
    }

    WNDCLASSEX wc    = {sizeof(WNDCLASSEX)};
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = PopupWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_SIZEALL);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"NetMonitorWidget";
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm       = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APPICON));
    RegisterClassEx(&wc);

    int x, y;
    if (!LoadPosition(x, y)) {
        x = GetSystemMetrics(SM_CXSCREEN) - WIN_W - 24;
        y = 24;
    }

    g_hPopup = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"NetMonitorWidget", L"",
        WS_POPUP | WS_VISIBLE,
        x, y, WIN_W, WIN_H,
        NULL, NULL, hInst, NULL);

    if (!g_hPopup) return 1;

    g_updateThread = CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
