#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wchar.h>
#include <pdh.h>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <atomic>
#include <mutex>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "shell32.lib")

#define ID_TIMER_WATCHDOG  1
#define ID_TIMER_ZORDER    2
#define IDM_EXIT 101
#define WM_APP_UPDATE_METRICS (WM_APP + 1)
#include "resource.h"

HINSTANCE g_hInst;
HWND g_hPopup = NULL;
HWND g_hTaskbar = NULL;
int g_popupWidth = 132;
int g_popupHeight = 48;
int g_xOffset = 310;
int g_yOffset = 0;
int g_screenHeight = 0;
CRITICAL_SECTION g_cs;
PDH_HQUERY hQuery = nullptr;
PDH_HCOUNTER hNetDown = nullptr, hNetUp = nullptr;
HFONT g_hFont = nullptr;
HANDLE g_hMutex = NULL;

std::atomic<bool> g_keepRunning{ true };
HANDLE g_updateThread = nullptr;
std::wstring g_metricsText = L"Loading...";

RECT g_lastTaskbarRect = {0};

// Хук на события перемещения таскбара
HWINEVENTHOOK g_hEventHook = nullptr;

LRESULT CALLBACK PopupWndProc(HWND, UINT, WPARAM, LPARAM);
bool FindTaskbar();
void UpdatePopupPositionNow();

std::wstring GetMetricsText();

// --------------------------------------------------------------------------
// Отрисовка через UpdateLayeredWindow
// --------------------------------------------------------------------------
void UpdateLayeredPopup(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width  = rc.right  - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;
    if (!g_hFont) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    ReleaseDC(NULL, hdcScreen);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   pBits   = NULL;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBitmap) { DeleteDC(hdcMem); return; }

    SelectObject(hdcMem, hBitmap);
    memset(pBits, 0, width * height * 4);

    SetTextColor(hdcMem, RGB(255, 255, 255));
    SetBkMode(hdcMem, TRANSPARENT);

    EnterCriticalSection(&g_cs);
    std::wstring text = g_metricsText;
    LeaveCriticalSection(&g_cs);

    HFONT hOldFont = (HFONT)SelectObject(hdcMem, g_hFont);
    RECT rcText = rc;
    DrawTextW(hdcMem, text.c_str(), -1, &rcText, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
    int textHeight = rcText.bottom - rcText.top;
    int yOff       = (height - textHeight) / 2;
    RECT rcDraw    = rc;
    rcDraw.top    += yOff;
    rcDraw.bottom  = rcDraw.top + textHeight;
    DrawTextW(hdcMem, text.c_str(), -1, &rcDraw, DT_WORDBREAK | DT_CENTER);
    SelectObject(hdcMem, hOldFont);

    DWORD* pixels = (DWORD*)pBits;
    for (int i = 0; i < width * height; ++i) {
        DWORD pix = pixels[i];
        BYTE r = (pix >> 16) & 0xFF;
        BYTE g = (pix >>  8) & 0xFF;
        BYTE b =  pix        & 0xFF;
        BYTE brightness = max(max(r, g), b);
        if (brightness > 0) {
            BYTE alpha = brightness;
            if (alpha < 35)
                alpha = 0;
            else
                alpha = (BYTE)((alpha - 35) * 255 / (255 - 35));
            pixels[i] = (alpha << 24);
        } else {
            pixels[i] = 0x01000000;
        }
    }

    POINT ptDst = {rc.left, rc.top};
    SIZE  sizeWnd = {width, height};
    POINT ptSrc   = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ClientToScreen(hwnd, &ptDst);
    UpdateLayeredWindow(hwnd, NULL, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
}

// --------------------------------------------------------------------------
// Фоновый поток обновления метрик
// --------------------------------------------------------------------------
DWORD WINAPI UpdateThread(LPVOID) {
    while (g_keepRunning) {
        std::wstring text = GetMetricsText();
        EnterCriticalSection(&g_cs);
        g_metricsText = text;
        LeaveCriticalSection(&g_cs);
        if (g_hPopup) {
            PostMessageW(g_hPopup, WM_APP_UPDATE_METRICS, 0, 0);
        }
        Sleep(1000);
    }
    return 0;
}

// --------------------------------------------------------------------------
// Вспомогательная функция: немедленно поднять виджет поверх таскбара
// --------------------------------------------------------------------------
static inline void BringWidgetToTop() {
    if (g_hPopup)
        SetWindowPos(g_hPopup, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
}

// --------------------------------------------------------------------------
// WinEvent-колбэк: перемещение таскбара (только поток таскбара)
// --------------------------------------------------------------------------
void CALLBACK WinEventProcLocation(
    HWINEVENTHOOK /*hHook*/, DWORD event,
    HWND hwnd, LONG idObject, LONG /*idChild*/,
    DWORD /*dwEventThread*/, DWORD /*dwmsEventTime*/)
{
    if (event == EVENT_OBJECT_LOCATIONCHANGE &&
        hwnd == g_hTaskbar &&
        idObject == OBJID_WINDOW)
    {
        UpdatePopupPositionNow();
        BringWidgetToTop();
    }
}

// --------------------------------------------------------------------------
// Регистрация хука на перемещение таскбара
// --------------------------------------------------------------------------
void InstallEventHook() {
    if (!g_hEventHook && g_hTaskbar) {
        DWORD taskbarThreadId = GetWindowThreadProcessId(g_hTaskbar, nullptr);
        g_hEventHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            NULL,
            WinEventProcLocation,
            0,
            taskbarThreadId,
            WINEVENT_OUTOFCONTEXT
        );
    }
}

// --------------------------------------------------------------------------
// PDH
// --------------------------------------------------------------------------
PDH_STATUS InitPDH() {
    hNetDown = hNetUp = nullptr;
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &hQuery);
    if (status != ERROR_SUCCESS) return status;

    if (PdhAddEnglishCounterW(hQuery,
            L"\\Network Interface(*)\\Bytes Received/sec", 0, &hNetDown) != ERROR_SUCCESS)
        hNetDown = nullptr;

    if (PdhAddEnglishCounterW(hQuery,
            L"\\Network Interface(*)\\Bytes Sent/sec", 0, &hNetUp) != ERROR_SUCCESS)
        hNetUp = nullptr;

    PdhCollectQueryData(hQuery);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    PdhCollectQueryData(hQuery);
    return ERROR_SUCCESS;
}

// --------------------------------------------------------------------------
// Суммирует значения wildcard-счётчика по всем интерфейсам
// --------------------------------------------------------------------------
static double SumCounterArray(PDH_HCOUNTER hCounter) {
    if (!hCounter) return 0.0;

    DWORD bufSize = 0, itemCount = 0;
    PDH_STATUS st = PdhGetFormattedCounterArray(hCounter, PDH_FMT_DOUBLE,
                                                &bufSize, &itemCount, nullptr);
    if (st != (PDH_STATUS)0x800007D2L && st != ERROR_SUCCESS)
        return 0.0;

    std::vector<BYTE> buf(bufSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    st = PdhGetFormattedCounterArray(hCounter, PDH_FMT_DOUBLE,
                                     &bufSize, &itemCount, items);
    if (st != ERROR_SUCCESS)
        return 0.0;

    double total = 0.0;
    for (DWORD i = 0; i < itemCount; ++i) {
        double val = items[i].FmtValue.doubleValue;
        if (val >= 0.0 && !std::isnan(val) && !std::isinf(val))
            total += val;
    }
    return total;
}

std::wstring GetMetricsText() {
    PdhCollectQueryData(hQuery);

    // Суммируем трафик по всем сетевым интерфейсам
    double netDown = SumCounterArray(hNetDown) / 1024.0;
    double netUp   = SumCounterArray(hNetUp)   / 1024.0;

    wchar_t downUnit = L'K';
    if (netDown >= 1024.0) { netDown /= 1024.0; downUnit = L'M'; }

    wchar_t upUnit = L'K';
    if (netUp >= 1024.0) { netUp /= 1024.0; upUnit = L'M'; }

    wchar_t buf[128];
    swprintf_s(buf, L"D:%5.1f %cB/s\nU:%5.1f %cB/s", netDown, downUnit, netUp, upUnit);
    return std::wstring(buf);
}

// --------------------------------------------------------------------------
// WinMain
// --------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
	
	g_hMutex = CreateMutexW(NULL, TRUE, L"TaskbarWidgetPDH_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_hMutex);
        return 0;
    }
    g_hInst       = hInstance;
    g_screenHeight = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    InitPDH();

    WNDCLASSEX wc     = {sizeof(WNDCLASSEX)};
    wc.style          = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc    = PopupWndProc;
    wc.hInstance      = hInstance;
    wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground  = NULL;
    wc.lpszClassName  = L"TaskbarWidgetPDH";
    wc.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm        = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    RegisterClassEx(&wc);

    FindTaskbar();

    RECT taskbarRect = {0};
    if (g_hTaskbar) {
        GetWindowRect(g_hTaskbar, &taskbarRect);

        HWND hTrayNotifyWnd = FindWindowEx(g_hTaskbar, NULL, L"TrayNotifyWnd", NULL);
        if (hTrayNotifyWnd) {
            RECT trayNotifyRect = {0};
            GetWindowRect(hTrayNotifyWnd, &trayNotifyRect);
            if (taskbarRect.right > 0 && trayNotifyRect.left > 0)
                g_xOffset = taskbarRect.right - trayNotifyRect.left;
        }
    }

    int x = taskbarRect.right - g_popupWidth - g_xOffset;
    int y = taskbarRect.top   + g_yOffset;

    g_hPopup = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"TaskbarWidgetPDH", L"",
        WS_POPUP | WS_VISIBLE,
        x, y, g_popupWidth, g_popupHeight,
        NULL, NULL, hInstance, NULL);

    if (!g_hPopup) {
        MessageBoxW(NULL, L"Ошибка создания окна", L"Ошибка", MB_ICONERROR);
        return 0;
    }

    // Устанавливаем хук на события таскбара
    InstallEventHook();

    // Watchdog: раз в 500 мс проверяем, жив ли таскбар
    SetTimer(g_hPopup, ID_TIMER_WATCHDOG, 500, NULL);
    SetTimer(g_hPopup, ID_TIMER_ZORDER, 1, NULL);

    g_updateThread = CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

// --------------------------------------------------------------------------
bool FindTaskbar() {
    g_hTaskbar = FindWindow(L"Shell_TrayWnd", NULL);
    return g_hTaskbar != NULL;
}

void UpdatePopupPositionNow() {
    if (!g_hTaskbar) {
        FindTaskbar();
        if (!g_hTaskbar) return;
    }

    RECT taskbarRect;
    if (!GetWindowRect(g_hTaskbar, &taskbarRect)) return;

    if (taskbarRect.top    == g_lastTaskbarRect.top    &&
        taskbarRect.bottom == g_lastTaskbarRect.bottom &&
        taskbarRect.right  == g_lastTaskbarRect.right)
        return;

    int x = taskbarRect.right - g_popupWidth - g_xOffset;
    int y = taskbarRect.top   + g_yOffset;

    SetWindowPos(g_hPopup, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);

    g_lastTaskbarRect = taskbarRect;
}

HFONT CreateMono15Light() {
    return CreateFontW(
        -12, 0, 0, 0, 400,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FIXED_PITCH,
        L"Courier New");
}

// --------------------------------------------------------------------------
// Оконная процедура
// --------------------------------------------------------------------------
LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hFont = CreateMono15Light();
            UpdateLayeredPopup(hwnd);
            return 0;

        case WM_TIMER:
            if (wParam == ID_TIMER_ZORDER) {
                // Если таскбар на переднем плане - немедленно поднимаем виджет.
                 if (g_hTaskbar && ::GetForegroundWindow() == g_hTaskbar)
                    BringWidgetToTop();
                break;
            }
            if (wParam == ID_TIMER_WATCHDOG) {
                // Если таскбар пропал (Explorer перезапустился) - переподключаемся
                if (!g_hTaskbar || !IsWindow(g_hTaskbar)) {
                    if (g_hEventHook) {
                        UnhookWinEvent(g_hEventHook);
                        g_hEventHook = nullptr;
                    }
                    g_hTaskbar = NULL;
                    if (FindTaskbar()) {
                        RECT taskbarRect = {0}, trayNotifyRect = {0};
                        GetWindowRect(g_hTaskbar, &taskbarRect);
                        HWND hTray = FindWindowEx(g_hTaskbar, NULL, L"TrayNotifyWnd", NULL);
                        if (hTray) {
                            GetWindowRect(hTray, &trayNotifyRect);
                            if (taskbarRect.right > 0 && trayNotifyRect.left > 0)
                                g_xOffset = taskbarRect.right - trayNotifyRect.left;
                        }
                        InstallEventHook();
                        UpdatePopupPositionNow();
                    }
                }
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDM_EXIT)
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;

        case WM_CONTEXTMENU: {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Закрыть");
            POINT pt;
            GetCursorPos(&pt);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
            return 0;
        }

        case WM_LBUTTONDBLCLK:
            // Симулируем Ctrl+Shift+Esc
            keybd_event(VK_CONTROL, 0, 0, 0);
            keybd_event(VK_SHIFT,   0, 0, 0);
            keybd_event(VK_ESCAPE,  0, 0, 0);
            keybd_event(VK_ESCAPE,  0, KEYEVENTF_KEYUP, 0);
            keybd_event(VK_SHIFT,   0, KEYEVENTF_KEYUP, 0);
            keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_NCHITTEST:
            return HTCLIENT;

        case WM_APP_UPDATE_METRICS:
            UpdateLayeredPopup(hwnd);
            return 0;

        case WM_DESTROY:
            // Останавливаем фоновый поток
            g_keepRunning = false;
            if (g_updateThread) {
                WaitForSingleObject(g_updateThread, 2000);
                CloseHandle(g_updateThread);
                g_updateThread = nullptr;
            }
            // Снимаем WinEvent-хук
            if (g_hEventHook) {
                UnhookWinEvent(g_hEventHook);
                g_hEventHook = nullptr;
            }
            // PDH cleanup
            if (hQuery) {
                PdhCloseQuery(hQuery);
                hQuery = nullptr;
            }
            KillTimer(hwnd, ID_TIMER_WATCHDOG);
            KillTimer(hwnd, ID_TIMER_ZORDER);
			
            if (g_hFont) {
                DeleteObject(g_hFont);
                g_hFont = nullptr;
            }
			if (g_hMutex) {
                ReleaseMutex(g_hMutex);
                CloseHandle(g_hMutex);
                g_hMutex = nullptr;
            }
            DeleteCriticalSection(&g_cs);
            PostQuitMessage(0);
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
