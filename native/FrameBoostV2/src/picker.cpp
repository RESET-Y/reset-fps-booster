// LET THE PERSON SAY WHICH WINDOW.
//
// Guessing was wrong twice. GetForegroundWindow() at start-up returns the app
// that launched us, because the user has just clicked its button; skipping the
// launcher and waiting for something else is better but still a guess, and it
// picks whatever happens to come forward next.
//
// The picker belongs in the RFB window itself. It cannot go there yet: that is
// C#, and this machine has the .NET runtimes but no SDK, so the app cannot be
// rebuilt. This is the same choice offered from the engine instead - it opens
// the moment the button is pressed, so the flow is unchanged from where the
// user stands.
//
// When the app can be rebuilt, RFB passes "hwnd <value>" and this never
// appears. That path is already wired below, so the integration is a change in
// the app and none here.
#include "picker.h"
#include "logger.h"

#include <commctrl.h>
#include <psapi.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace fbv2 {

namespace {

struct Candidate {
    HWND hwnd = nullptr;
    std::wstring label;
};

std::wstring ProcessNameOf(HWND hwnd, DWORD& outPid) {
    outPid = 0;
    GetWindowThreadProcessId(hwnd, &outPid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, outPid);
    if (!process) return L"";
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &len) != 0;
    CloseHandle(process);
    if (!ok) return L"";
    std::wstring exe(path);
    const size_t slash = exe.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? exe : exe.substr(slash + 1);
}

bool IsCloaked(HWND hwnd) {
    // UWP keeps invisible placeholder windows around that pass IsWindowVisible.
    // DWM knows they are cloaked; without this the list fills with ghosts.
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static Fn getAttr = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<Fn>(GetProcAddress(dwm, "DwmGetWindowAttribute")) : nullptr;
    }();
    if (!getAttr) return false;
    int cloaked = 0;
    return SUCCEEDED(getAttr(hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked)))
           && cloaked != 0;
}

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
    auto* list = reinterpret_cast<std::vector<Candidate>*>(param);

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;   // dialogs, not apps
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (IsCloaked(hwnd)) return TRUE;

    wchar_t title[256] = {};
    if (GetWindowTextW(hwnd, title, 255) <= 0) return TRUE;

    DWORD pid = 0;
    std::wstring exe = ProcessNameOf(hwnd, pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    std::wstring lower = exe;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    if (lower == L"resetfpsbooster.exe") return TRUE;

    RECT r{};
    GetWindowRect(hwnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    // A window too small to play in is not the game. Keeps the list short
    // enough to read at a glance, which is the whole point of a picker.
    if (w < 320 || h < 240) return TRUE;

    Candidate c;
    c.hwnd = hwnd;
    c.label = std::wstring(title) + L"   [" + exe + L"]   "
            + std::to_wstring(w) + L"x" + std::to_wstring(h);
    list->push_back(std::move(c));
    return TRUE;
}

constexpr wchar_t kPickerClass[] = L"ResetFrameBoostV2Picker";
constexpr int kIdList = 101, kIdOk = 102, kIdCancel = 103;

HWND g_list = nullptr;
HWND g_chosen = nullptr;
bool g_done = false;

void Choose(const std::vector<Candidate>& items) {
    const int sel = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
    if (sel >= 0 && sel < static_cast<int>(items.size())) g_chosen = items[sel].hwnd;
    g_done = true;
}

LRESULT CALLBACK PickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* items = reinterpret_cast<std::vector<Candidate>*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_COMMAND:
        if (!items) break;
        if (LOWORD(wp) == kIdOk
            || (LOWORD(wp) == kIdList && HIWORD(wp) == LBN_DBLCLK)) {
            Choose(*items);
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == kIdCancel) { g_done = true; DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        g_done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_done = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

HWND PickWindow() {
    std::vector<Candidate> items;
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&items));

    if (items.empty()) {
        Logger::Log("[FrameBoostV2] Picker: no candidate windows found.");
        return nullptr;
    }
    if (items.size() == 1) {
        const std::wstring& l = items[0].label;
        Logger::Log("[FrameBoostV2] Picker: only one candidate, taking it: "
                    + std::string(l.begin(), l.end()));
        return items[0].hwnd;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PickerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kPickerClass;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    const int width = 680, height = 420;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kPickerClass,
                                L"RESET FrameBoost - welches Fenster soll geboostet werden?",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                x, y, width, height,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&items));

    RECT client{};
    GetClientRect(hwnd, &client);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                             12, 12, client.right - 24, client.bottom - 70,
                             hwnd, reinterpret_cast<HMENU>(kIdList),
                             GetModuleHandleW(nullptr), nullptr);

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    for (const auto& c : items)
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.label.c_str()));
    SendMessageW(g_list, LB_SETCURSEL, 0, 0);

    HWND ok = CreateWindowExW(0, L"BUTTON", L"Boosten",
                              WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                              client.right - 232, client.bottom - 46, 100, 30,
                              hwnd, reinterpret_cast<HMENU>(kIdOk),
                              GetModuleHandleW(nullptr), nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Abbrechen",
                                  WS_CHILD | WS_VISIBLE,
                                  client.right - 120, client.bottom - 46, 100, 30,
                                  hwnd, reinterpret_cast<HMENU>(kIdCancel),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(g_list);

    g_chosen = nullptr;
    g_done = false;

    // DRAIN ANY WM_QUIT ALREADY IN THE QUEUE BEFORE LOOPING.
    //
    // The first run of this picker logged "cancelled" one second after it
    // opened, without anybody touching it. A GetMessage loop ends on WM_QUIT
    // whatever posted it, and a quit posted before the loop starts is
    // indistinguishable from the user closing the window. Clearing the queue
    // first makes the loop answer only to this window.
    {
        MSG stale;
        while (PeekMessageW(&stale, nullptr, 0, 0, PM_REMOVE)) {}
    }

    MSG msg;
    BOOL got = 0;
    while (!g_done && (got = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == g_list) {
            if (msg.wParam == VK_RETURN) { Choose(items); DestroyWindow(hwnd); continue; }
            if (msg.wParam == VK_ESCAPE) { g_done = true; DestroyWindow(hwnd); continue; }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (!g_done) {
        // The loop ended without the window saying so: GetMessage returned 0
        // (a WM_QUIT from elsewhere) or -1 (an error). Worth naming, because
        // silently reporting "cancelled" for it is what made the first
        // failure look like a user action.
        Logger::Log(std::string("[FrameBoostV2] Picker: message loop ended early, ")
                    + "GetMessage returned " + std::to_string(got)
                    + " (0 = WM_QUIT from elsewhere, -1 = error "
                    + std::to_string(GetLastError()) + ").");
    }

    // Drain whatever the destroy posted, so the engine's own loop starts clean.
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {}

    if (g_chosen) {
        wchar_t title[256] = {};
        GetWindowTextW(g_chosen, title, 255);
        const std::wstring w(title);
        Logger::Log("[FrameBoostV2] Picker: chosen window: "
                    + std::string(w.begin(), w.end()));
    } else {
        Logger::Log("[FrameBoostV2] Picker: cancelled.");
    }
    return g_chosen;
}

} // namespace fbv2
