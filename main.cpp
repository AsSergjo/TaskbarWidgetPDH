#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wchar.h>
#include <pdh.h>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "shell32.lib")

#define ID_TIMER_SYNC 1
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
PDH_HCOUNTER hCpu = nullptr, hMem = nullptr, hDisk = nullptr, hNet = nullptr;
HFONT g_hFont = nullptr;

bool g_keepRunning = true;
HANDLE g_updateThread = nullptr;
std::wstring g_metricsText = L"Loading...";

RECT g_lastTaskbarRect = {0};

LRESULT CALLBACK PopupWndProc(HWND, UINT, WPARAM, LPARAM);
bool FindTaskbar();
void UpdatePopupPositionNow();
const wchar_t* formatNetSpeed(double kbPerSec);

// Предварительное объявление, чтобы функция была видна в потоке
std::wstring GetMetricsText();

// Функция для обновления окна через UpdateLayeredWindow
void UpdateLayeredPopup(HWND hwnd) {
    // Получаем размеры клиентской области
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    // Проверяем, что шрифт создан
    if (!g_hFont) return;

    // Создаём memory DC
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    ReleaseDC(NULL, hdcScreen);

    // Создаём DIB секцию с 32bpp ARGB (формат BGRA)
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // отрицательная высота для top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBitmap) {
        DeleteDC(hdcMem);
        return;
    }
    SelectObject(hdcMem, hBitmap);
    // Заполняем фон полностью прозрачным (alpha = 0, цвет не важен)
    memset(pBits, 0, width * height * 4);
    // Рисуем текст белым цветом, чтобы яркость пикселей была >0
    SetTextColor(hdcMem, RGB(255, 255, 255));
    SetBkMode(hdcMem, TRANSPARENT);

    EnterCriticalSection(&g_cs);
    std::wstring text = g_metricsText;
    LeaveCriticalSection(&g_cs);

    HFONT hOldFont = (HFONT)SelectObject(hdcMem, g_hFont);
    // Рассчитываем высоту текста
    RECT rcText = rc;
    DrawTextW(hdcMem, text.c_str(), -1, &rcText, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
    int textHeight = rcText.bottom - rcText.top;
    int yOffset = (height - textHeight) / 2;

    RECT rcDraw = rc;
    rcDraw.top += yOffset;
    rcDraw.bottom = rcDraw.top + textHeight;
    // Рисуем текст
    DrawTextW(hdcMem, text.c_str(), -1, &rcDraw, DT_WORDBREAK | DT_CENTER);
    SelectObject(hdcMem, hOldFont);

    DWORD* pixels = (DWORD*)pBits;
    for (int i = 0; i < width * height; ++i) {
        DWORD pix = pixels[i];
        BYTE r = (pix >> 16) & 0xFF;
        BYTE g = (pix >> 8) & 0xFF;
        BYTE b = pix & 0xFF;

        BYTE brightness = max(max(r, g), b);
        if (brightness > 0) {
            BYTE alpha = brightness;
            // жёстко отрезаем пушок у букв
            if (alpha < 35)
                alpha = 0;
            else
                alpha = (alpha - 35) * 255 / (255 - 35);
            pixels[i] = (alpha << 24); // цвет чёрный (0,0,0)
        } else {
            // Фоновый пиксель: устанавливаем альфа = 1 (почти прозрачный), чтобы мышь работала
            pixels[i] = 0x01000000; // альфа=1, цвет чёрный
        }
    }
    // Вызываем UpdateLayeredWindow с явными позицией и размером
    POINT ptDst = {rc.left, rc.top};
    SIZE sizeWnd = {width, height};
    POINT ptSrc = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA}; // Используем альфа-канал пикселей
    // Конвертируем клиентские координаты в экранные
    ClientToScreen(hwnd, &ptDst);
    UpdateLayeredWindow(hwnd, NULL, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    // Очистка
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
}

const wchar_t* formatNetSpeed(double net) {
    static wchar_t netStr[32];
   
if (net >= 1000.0) {//уже net в КБ
    // Конвертируем в МБ/с (1 МБ = 1024 КБ)
    double netMB = net / 1024.0;
    swprintf_s(netStr, sizeof(netStr)/sizeof(netStr[0]), L"%6.1f MB/s", netMB);
} else if (net >= 100.0) {
    // Можно показывать с одним десятичным знаком для больших значений в КБ/с
    swprintf_s(netStr, sizeof(netStr)/sizeof(netStr[0]), L"%6.1f KB/s", net);
} else {
    // Для значений меньше 1000 показываем без десятичных
    swprintf_s(netStr, sizeof(netStr)/sizeof(netStr[0]), L"%6.1f KB/s", net);
}

    return netStr;
}

// Функция для фонового потока обновления метрик
DWORD WINAPI UpdateThread(LPVOID) {
    while (g_keepRunning) {
        std::wstring text = GetMetricsText();
        EnterCriticalSection(&g_cs);
        g_metricsText = text;
        LeaveCriticalSection(&g_cs);
        // Вызываем перерисовку из фонового потока
        if (g_hPopup) {
            // Используем PostMessage для потокобезопасного обновления
            PostMessageW(g_hPopup, WM_APP_UPDATE_METRICS, 0, 0);
        }
        Sleep(1000);
    }
    return 0;
}

// Инициализация PDH-счётчиков
PDH_STATUS InitPDH() {
    PDH_STATUS status;
    // Явно инициализируем дескрипторы, чтобы избежать использования мусорных значений
    hCpu = hMem = hDisk = hNet = nullptr;

    status = PdhOpenQueryW(nullptr, 0, &hQuery);
    if (status != ERROR_SUCCESS) return status;
    // Используем PdhAddEnglishCounterW для независимости от языка системы
    // CPU
    if (PdhAddEnglishCounterW(hQuery, L"\\Processor(_Total)\\% Processor Time", 0, &hCpu) != ERROR_SUCCESS) {
       
        hCpu = nullptr;
    }
    // Memory
    if (PdhAddEnglishCounterW(hQuery, L"\\Memory\\% Committed Bytes In Use", 0, &hMem) != ERROR_SUCCESS) {
       
        hMem = nullptr;
    }
    // Disk
    if (PdhAddEnglishCounterW(hQuery, L"\\LogicalDisk(_Total)\\% Idle Time", 0, &hDisk) != ERROR_SUCCESS) {
        
        hDisk = nullptr;
    }
    // Network - ищем первый доступный интерфейс
   // Пробуем найти подходящий сетевой счетчик
    if (PdhAddEnglishCounterW(hQuery, L"\\Network Interface(*)\\Bytes Total/sec", 0, &hNet) != ERROR_SUCCESS) {
        
        hNet = nullptr;
    }

    // Если ни один счётчик не добавлен, считаем это ошибкой, но продолжаем выполнение
    if (!hCpu && !hMem && !hDisk && !hNet) {
        
        // Не будем возвращать ошибку, чтобы приложение не падало, а показывало пустые значения
    }

    PdhCollectQueryData(hQuery); // Первый сбор данных
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Небольшая задержка
    PdhCollectQueryData(hQuery); // Второй сбор для получения валидных значений
    return ERROR_SUCCESS;
}

// Получить строку с метриками
std::wstring GetMetricsText() {
    PdhCollectQueryData(hQuery);
    PDH_FMT_COUNTERVALUE v;
    double cpu = 0, mem = 0, disk = 0, net = 0;

    if (hCpu && PdhGetFormattedCounterValue(hCpu, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
        cpu = v.doubleValue;
        if (cpu < 0.0) cpu = 0.0;
        if (cpu > 100.0) cpu = 99.9;
    }
    if (hMem && PdhGetFormattedCounterValue(hMem, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
        mem = v.doubleValue;
        if (mem < 0.0) mem = 0.0;
        if (mem > 100.0) mem = 99.9;
    }
    if (hDisk && PdhGetFormattedCounterValue(hDisk, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
        double idle = v.doubleValue;
        if (idle < 0.0) idle = 0.0;
        if (idle > 100.0) idle = 99.9;
        disk = 100.0 - idle;
    }
    if (hNet && PdhGetFormattedCounterValue(hNet, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
        net = v.doubleValue;
        if (!std::isnan(net) && !std::isinf(net) && net >= 0.0) {
            net /= 1024.0; // КБ/с
        } else {
            net = 0.0;
        }
    }

    wchar_t buf[128];
    swprintf_s(buf, sizeof(buf)/sizeof(buf[0]),
    L"%s D:%3.1f\n CPU:%4.1f RAM:%3.0f", formatNetSpeed(net), disk, cpu, mem);
    return std::wstring(buf);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    g_hInst = hInstance;
    g_screenHeight = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    InitPDH(); // Инициализируем PDH, но не проверяем ошибку, чтобы не падать
    
    WNDCLASSEX wc = {sizeof(WNDCLASSEX)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = PopupWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"TaskbarWidgetPDH";
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    RegisterClassEx(&wc);
    
    FindTaskbar();
        
    RECT taskbarRect = {0};
    if (g_hTaskbar) {
        GetWindowRect(g_hTaskbar, &taskbarRect);
        
        HWND hTrayNotifyWnd = FindWindowEx(g_hTaskbar, NULL, L"TrayNotifyWnd", NULL);
        RECT trayNotifyRect = {0};
        if (hTrayNotifyWnd) {
            GetWindowRect(hTrayNotifyWnd, &trayNotifyRect);
            if (taskbarRect.right > 0 && trayNotifyRect.left > 0) {
                // Рассчитываем смещение так, чтобы окно было слева от области уведомлений
                g_xOffset = taskbarRect.right - trayNotifyRect.left;
            }
        }
    }
    
    int x = taskbarRect.right - g_popupWidth - g_xOffset;
    int y = taskbarRect.top + g_yOffset;
    
    g_hPopup = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"TaskbarWidgetPDH",
        L"",
        WS_POPUP | WS_VISIBLE,
        x, y, g_popupWidth, g_popupHeight,
        NULL, NULL, hInstance, NULL
    );
    if(!g_hPopup){
        MessageBoxW(NULL,L"Ошибка создания окна",L"Ошибка", MB_ICONERROR);
        return 0;
    }
    SetTimer(g_hPopup, ID_TIMER_SYNC, 1, NULL);
  
    // Запускаем фоновый поток для сбора данных
    g_updateThread = CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr);
           
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
       
return 0;
}

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
    if (!GetWindowRect(g_hTaskbar, &taskbarRect)) {
        return;
    }
    
    if (taskbarRect.top == g_lastTaskbarRect.top &&
        taskbarRect.bottom == g_lastTaskbarRect.bottom &&
        taskbarRect.right == g_lastTaskbarRect.right) {
        return;
    }
    
    int x = taskbarRect.right - g_popupWidth - g_xOffset;
    int y = taskbarRect.top + g_yOffset;
    
    SetWindowPos(g_hPopup, HWND_TOPMOST, x, y, 0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
      
    g_lastTaskbarRect = taskbarRect;
}

HFONT CreateMono15Light()
{    
     return CreateFontW(
    -12,              // !!!
    0, 0, 0,
    400,
    FALSE, FALSE, FALSE,
    DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS,
    ANTIALIASED_QUALITY,
    FIXED_PITCH,
    L"Courier New");
    
}

LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        
    switch (msg) {
        case WM_CREATE: {
            g_hFont = CreateMono15Light();
            // Первоначальная отрисовка текста
            UpdateLayeredPopup(hwnd);
            return 0;
        }
        case WM_TIMER:
            if (wParam == ID_TIMER_SYNC) {
                UpdatePopupPositionNow();
                if (g_hTaskbar){
                    if (::GetForegroundWindow() == g_hTaskbar)
                    {
                        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
                    }
                }
            }
            break;
            
        case WM_COMMAND:
            if (LOWORD(wParam) == IDM_EXIT) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
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
        
        case WM_LBUTTONDBLCLK: {
            // Симулируем Ctrl+Shift+Esc
            keybd_event(VK_CONTROL, 0, 0, 0);
            keybd_event(VK_SHIFT, 0, 0, 0);
            keybd_event(VK_ESCAPE, 0, 0, 0);
            keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
            keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
            keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
            return 0;
        }
        case WM_PAINT: {
            // WM_PAINT не используется, но обрабатываем,
            // чтобы избежать лишних сообщений о невалидной области.
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_ERASEBKGND:
            return 1; 
        case WM_NCHITTEST:
            // Делаем всё окно кликабельным (включая прозрачные области)
            return HTCLIENT;
        case WM_APP_UPDATE_METRICS:
            UpdateLayeredPopup(hwnd);
            return 0;
            
        case WM_DESTROY:
            
            KillTimer(hwnd, ID_TIMER_SYNC);
            if (g_hFont) {
                DeleteObject(g_hFont);
                g_hFont = nullptr;
            }            
            PostQuitMessage(0);
            break;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
