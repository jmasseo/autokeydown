// AutoKeyDown - latch held keys when Scroll Lock is turned on.
//
// Behaviour:
//   * When Scroll Lock is switched ON, every key that is physically held at that
//     moment becomes "preserved": when you let go of it, the key-up is swallowed,
//     so Windows and every application keep seeing the key as held down.
//   * The preserved state ends when either
//       - you press any one of the preserved keys again, or
//       - Scroll Lock is switched OFF.
//     All preserved keys are then released (a key-up is sent for each one).
//   * Optionally (on by default, see tray menu) Scroll Lock is switched off
//     automatically when the state ends via a key press, so the Scroll Lock LED
//     always shows whether keys are being preserved.
//
// Implementation: a low-level keyboard hook (WH_KEYBOARD_LL) tracks physical key
// state and suppresses key-ups of preserved keys. Synthetic key-ups are sent with
// SendInput from the UI thread (never from inside the hook callback).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

namespace {

constexpr ULONG_PTR kInjectTag = 0x414B4431;  // 'AKD1' marks our own SendInput events
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RELEASE_LATCHED = WM_APP + 2;  // wParam: 1 = also turn Scroll Lock off
constexpr UINT WM_STATE_CHANGED = WM_APP + 3;
constexpr UINT kTrayId = 1;
constexpr UINT IDM_STATUS = 100;
constexpr UINT IDM_AUTO_SCROLL_OFF = 101;
constexpr UINT IDM_EXIT = 102;

struct KeyInfo {
    DWORD scanCode = 0;
    bool extended = false;
};

// All of this state is touched only on the UI thread (the LL hook callback runs
// on the thread that installed it, inside its message loop), so no locking needed.
bool g_physDown[256] = {};
bool g_latched[256] = {};
KeyInfo g_keyInfo[256] = {};
bool g_scrollOn = false;
bool g_autoScrollOff = true;
bool g_acceptInjected = false;  // --accept-injected: treat synthetic input as physical (testing)

HHOOK g_hook = nullptr;
HWND g_wnd = nullptr;
HINSTANCE g_inst = nullptr;
HICON g_iconIdle = nullptr;
HICON g_iconActive = nullptr;
UINT g_wmTaskbarCreated = 0;

bool AnyLatched() {
    for (bool b : g_latched)
        if (b) return true;
    return false;
}

std::wstring KeyName(int vk) {
    const KeyInfo& ki = g_keyInfo[vk];
    LONG lp = static_cast<LONG>(ki.scanCode << 16);
    if (ki.extended) lp |= 1 << 24;
    wchar_t buf[64] = {};
    if (GetKeyNameTextW(lp, buf, 64) > 0) return buf;
    wchar_t fallback[16];
    wsprintfW(fallback, L"VK 0x%02X", vk);
    return fallback;
}

std::wstring LatchedList() {
    std::wstring s;
    for (int vk = 0; vk < 256; ++vk) {
        if (!g_latched[vk]) continue;
        if (!s.empty()) s += L", ";
        s += KeyName(vk);
    }
    return s;
}

void SendKey(WORD vk, const KeyInfo& ki, bool up) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(ki.scanCode);
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (ki.extended ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = kInjectTag;
    SendInput(1, &in, sizeof(in));
}

// Release every preserved key that is not physically held right now.
void ReleaseLatched() {
    for (int vk = 0; vk < 256; ++vk) {
        if (!g_latched[vk]) continue;
        g_latched[vk] = false;
        if (!g_physDown[vk]) SendKey(static_cast<WORD>(vk), g_keyInfo[vk], true);
    }
}

void SetScrollLockOff() {
    if (!g_scrollOn) return;
    KeyInfo ki{static_cast<DWORD>(MapVirtualKeyW(VK_SCROLL, MAPVK_VK_TO_VSC)), false};
    SendKey(VK_SCROLL, ki, false);
    SendKey(VK_SCROLL, ki, true);
    g_scrollOn = false;  // our injected events are ignored by the hook, so track it here
}

void UpdateTray(DWORD msg) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    bool active = AnyLatched();
    nid.hIcon = active ? g_iconActive : g_iconIdle;
    std::wstring tip = active ? L"AutoKeyDown - holding: " + LatchedList()
                              : std::wstring(L"AutoKeyDown - idle (hold keys, then press Scroll Lock)");
    if (tip.size() > 127) tip = tip.substr(0, 124) + L"...";
    lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(msg, &nid);
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) return CallNextHookEx(g_hook, code, wParam, lParam);

    const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (kb->dwExtraInfo == kInjectTag || kb->vkCode > 255 || kb->vkCode == VK_PACKET)
        return CallNextHookEx(g_hook, code, wParam, lParam);
    // Treat input injected by other software as pass-through; only physical keys latch.
    if ((kb->flags & LLKHF_INJECTED) && !g_acceptInjected)
        return CallNextHookEx(g_hook, code, wParam, lParam);

    const int vk = static_cast<int>(kb->vkCode);
    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    if (down) {
        const bool isRepeat = g_physDown[vk];
        g_keyInfo[vk] = KeyInfo{kb->scanCode, (kb->flags & LLKHF_EXTENDED) != 0};

        if (vk == VK_SCROLL) {
            if (!isRepeat) {
                g_scrollOn = !g_scrollOn;
                if (g_scrollOn) {
                    bool any = false;
                    for (int k = 0; k < 256; ++k) {
                        if (k != VK_SCROLL && g_physDown[k]) {
                            g_latched[k] = true;
                            any = true;
                        }
                    }
                    if (any) PostMessageW(g_wnd, WM_STATE_CHANGED, 0, 0);
                } else if (AnyLatched()) {
                    PostMessageW(g_wnd, WM_RELEASE_LATCHED, 0, 0);
                }
            }
            g_physDown[vk] = true;
            return CallNextHookEx(g_hook, code, wParam, lParam);
        }

        if (g_latched[vk] && !isRepeat) {
            // A preserved key was pressed again: end the preserved state.
            // This key is already "down" as far as Windows knows, so swallow this
            // key-down and let its upcoming physical key-up release it normally.
            g_latched[vk] = false;
            g_physDown[vk] = true;
            PostMessageW(g_wnd, WM_RELEASE_LATCHED, g_autoScrollOff ? 1 : 0, 0);
            return 1;
        }

        g_physDown[vk] = true;
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }

    // Key up.
    g_physDown[vk] = false;
    if (g_latched[vk]) return 1;  // swallow: keep the key held for the system
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

HICON MakeDotIcon(COLORREF fill) {
    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, cx, cy);
    HBITMAP mask = CreateBitmap(cx, cy, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);

    // Mask: white = transparent, black = opaque.
    HGDIOBJ old = SelectObject(dc, mask);
    RECT r = {0, 0, cx, cy};
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SelectObject(dc, GetStockObject(BLACK_BRUSH));
    SelectObject(dc, GetStockObject(BLACK_PEN));
    RoundRect(dc, 1, 1, cx - 1, cy - 1, cx / 3, cy / 3);

    SelectObject(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
    SelectObject(dc, brush);
    SelectObject(dc, pen);
    RoundRect(dc, 1, 1, cx - 1, cy - 1, cx / 3, cy / 3);
    // A small "key cap" glyph: a down arrow.
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT font = CreateFontW(-(cy * 3 / 4), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             ANTIALIASED_QUALITY, 0, L"Segoe UI Symbol");
    HGDIOBJ oldFont = SelectObject(dc, font);
    DrawTextW(dc, L"↓", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    SelectObject(dc, old);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(font);
    DeleteObject(brush);
    DeleteObject(pen);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    return icon;
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    std::wstring status = AnyLatched() ? L"Holding: " + LatchedList() : std::wstring(L"Idle");
    if (status.size() > 80) status = status.substr(0, 77) + L"...";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_autoScrollOff ? MF_CHECKED : 0), IDM_AUTO_SCROLL_OFF,
                L"Turn Scroll Lock off when a held key is pressed");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_wmTaskbarCreated && g_wmTaskbarCreated != 0) {
        UpdateTray(NIM_ADD);  // Explorer restarted
        return 0;
    }
    switch (msg) {
        case WM_RELEASE_LATCHED:
            ReleaseLatched();
            if (wParam) SetScrollLockOff();
            UpdateTray(NIM_MODIFY);
            return 0;
        case WM_STATE_CHANGED:
            UpdateTray(NIM_MODIFY);
            return 0;
        case WM_TRAY:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU ||
                LOWORD(lParam) == WM_LBUTTONUP)
                ShowMenu();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_AUTO_SCROLL_OFF:
                    g_autoScrollOff = !g_autoScrollOff;
                    return 0;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;
        case WM_ENDSESSION:
        case WM_DESTROY:
            ReleaseLatched();
            if (g_hook) {
                UnhookWindowsHookEx(g_hook);
                g_hook = nullptr;
            }
            if (msg == WM_DESTROY) {
                UpdateTray(NIM_DELETE);
                PostQuitMessage(0);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--accept-injected")) g_acceptInjected = true;
    if (cmdLine && wcsstr(cmdLine, L"--no-auto-scroll-off")) g_autoScrollOff = false;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\AutoKeyDown.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"AutoKeyDown is already running (see the system tray).",
                    L"AutoKeyDown", MB_ICONINFORMATION);
        return 0;
    }

    g_inst = hInst;
    g_scrollOn = (GetKeyState(VK_SCROLL) & 1) != 0;
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"AutoKeyDownWnd";
    RegisterClassW(&wc);
    // A hidden top-level window (not message-only) so it receives TaskbarCreated.
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"AutoKeyDown", WS_POPUP, 0, 0, 0,
                            0, nullptr, nullptr, hInst, nullptr);
    if (!g_wnd) return 1;

    g_iconIdle = MakeDotIcon(RGB(120, 120, 120));
    g_iconActive = MakeDotIcon(RGB(0, 160, 70));

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    if (!g_hook) {
        MessageBoxW(nullptr, L"Failed to install the keyboard hook.", L"AutoKeyDown", MB_ICONERROR);
        return 1;
    }
    UpdateTray(NIM_ADD);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    DestroyIcon(g_iconIdle);
    DestroyIcon(g_iconActive);
    if (mutex) CloseHandle(mutex);
    return 0;
}
