// inputprobe: logs every keyboard/mouse event it receives, for checking macro playback.
//   inputprobe.exe <out.log> [seconds=6] [x=200] [y=200] [w=600] [h=400]
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static FILE* g_out = nullptr;
static LARGE_INTEGER g_qpf, g_t0;

static double Ms() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (t.QuadPart - g_t0.QuadPart) * 1000.0 / g_qpf.QuadPart; }

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    POINT pt; GetCursorPos(&pt);
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        fprintf(g_out, "%.1f key %s vk=%d sc=%d ext=%d\n", Ms(), (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) ? "down" : "up",
            (int)w, (int)((l >> 16) & 0xFF), (int)((l >> 24) & 1));
        return 0;
    case WM_CHAR: fprintf(g_out, "%.1f char %d\n", Ms(), (int)w); return 0;
    case WM_LBUTTONDOWN: fprintf(g_out, "%.1f mouse down left %ld %ld\n", Ms(), pt.x, pt.y); return 0;
    case WM_LBUTTONUP: fprintf(g_out, "%.1f mouse up left %ld %ld\n", Ms(), pt.x, pt.y); return 0;
    case WM_RBUTTONDOWN: fprintf(g_out, "%.1f mouse down right %ld %ld\n", Ms(), pt.x, pt.y); return 0;
    case WM_RBUTTONUP: fprintf(g_out, "%.1f mouse up right %ld %ld\n", Ms(), pt.x, pt.y); return 0;
    case WM_MBUTTONDOWN: fprintf(g_out, "%.1f mouse down middle %ld %ld\n", Ms(), pt.x, pt.y); return 0;
    case WM_MBUTTONUP: fprintf(g_out, "%.1f mouse up middle %ld %ld\n", Ms(), pt.x, pt.y); return 0;
    case WM_XBUTTONDOWN: fprintf(g_out, "%.1f mouse down x%d %ld %ld\n", Ms(), HIWORD(w), pt.x, pt.y); return TRUE;
    case WM_XBUTTONUP: fprintf(g_out, "%.1f mouse up x%d %ld %ld\n", Ms(), HIWORD(w), pt.x, pt.y); return TRUE;
    case WM_MOUSEWHEEL: fprintf(g_out, "%.1f wheel %d\n", Ms(), (int)(short)HIWORD(w)); return 0;
    case WM_MOUSEHWHEEL: fprintf(g_out, "%.1f hwheel %d\n", Ms(), (int)(short)HIWORD(w)); return 0;
    case WM_TIMER: PostQuitMessage(0); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); EndPaint(h, &ps); return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) return 1;
    g_out = _wfopen(argv[1], L"w");
    if (!g_out) return 2;
    double secs = argc > 2 ? _wtof(argv[2]) : 6;
    int x = argc > 3 ? _wtoi(argv[3]) : 200, y = argc > 4 ? _wtoi(argv[4]) : 200;
    int w = argc > 5 ? _wtoi(argv[5]) : 600, h = argc > 6 ? _wtoi(argv[6]) : 400;
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_t0);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"inputprobe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"inputprobe", L"inputprobe", WS_POPUP | WS_VISIBLE, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    SetTimer(hwnd, 1, (UINT)(secs * 1000), nullptr);
    fprintf(g_out, "0.0 ready %d %d %d %d\n", x, y, w, h);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    fclose(g_out);
    return 0;
}
