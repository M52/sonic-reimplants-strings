#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "sr_window.h"
#include "imgui.h"
#include "imgui_impl_win32.h"

#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

struct SrWindow {
    HWND hwnd;
    bool closed;
    bool backend;
    float scale;
    std::string drop;
};

static SrWindow* g_window;

static std::string utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n : 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(strlen(s.c_str()));
    return s;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_window && g_window->backend && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
    switch (msg) {
        case WM_CLOSE:
            if (g_window) g_window->closed = true;
            return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wparam;
            wchar_t path[4096];
            if (g_window && DragQueryFileW(drop, 0, path, 4096)) g_window->drop = utf8(path);
            DragFinish(drop);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lparam;
            float s = g_window ? g_window->scale : 1.0f;
            mm->ptMinTrackSize.x = (LONG)(620 * s);
            mm->ptMinTrackSize.y = (LONG)(480 * s);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

SrWindow* sr_window_open(const char* title, int width, int height) {
    ImGui_ImplWin32_EnableDpiAwareness();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"SonicReimplantsInstaller";
    RegisterClassExW(&wc);

    SrWindow* w = new SrWindow{};
    g_window = w;
    POINT origin = {0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    w->scale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
    if (w->scale < 1.0f) w->scale = 1.0f;
    RECT r = {0, 0, (LONG)(width * w->scale), (LONG)(height * w->scale)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int wide_title_len = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
    std::wstring wtitle((size_t)wide_title_len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title, -1, &wtitle[0], wide_title_len);
    w->hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!w->hwnd) {
        delete w;
        g_window = nullptr;
        return nullptr;
    }
    ShowWindow(w->hwnd, SW_SHOWDEFAULT);
    UpdateWindow(w->hwnd);
    return w;
}

void sr_window_close(SrWindow* w) {
    if (!w) return;
    if (w->backend) ImGui_ImplWin32_Shutdown();
    DestroyWindow(w->hwnd);
    g_window = nullptr;
    delete w;
}

float sr_window_scale(SrWindow* w) { return w->scale; }

bool sr_window_pump(SrWindow* w, int timeout_ms) {
    if (!w->backend && ImGui::GetCurrentContext()) {
        ImGui_ImplWin32_Init(w->hwnd);
        w->backend = true;
    }
    MSG msg;
    if (!PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE))
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)timeout_ms, QS_ALLINPUT);
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) w->closed = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (w->backend) ImGui_ImplWin32_NewFrame();
    return !w->closed;
}

void sr_window_size(SrWindow* w, int* width, int* height) {
    RECT r;
    GetClientRect(w->hwnd, &r);
    *width = r.right - r.left;
    *height = r.bottom - r.top;
}

void sr_window_present(SrWindow* w, const uint32_t* pixels, int width, int height) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(w->hwnd);
    SetDIBitsToDevice(dc, 0, 0, (DWORD)width, (DWORD)height, 0, 0, 0, (UINT)height, pixels, &bi, DIB_RGB_COLORS);
    ReleaseDC(w->hwnd, dc);
}

std::string sr_window_take_drop(SrWindow* w) {
    std::string d;
    d.swap(w->drop);
    return d;
}
