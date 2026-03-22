// Minimal Win32 tray application to pause/resume whitelisted games.
// Features:
// - Global hotkey (Pause) to toggle pause for the foreground process if whitelisted
// - Simple whitelist in "whitelist.txt" (one executable name per line)
// - Tray icon with context menu: add foreground to whitelist, toggle pulse mode, open whitelist, exit
// - Two pause modes: hard suspend/resume, and pulse mode which briefly resumes threads periodically

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <set>
#include <atomic>
#include <fstream>
#include <algorithm>

#include "resource.h"
// for higher-resolution sleep
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

const wchar_t* WHITELIST_FILE = L"whitelist.txt";
const UINT TRAY_ICON_ID = 100;
const UINT WM_TRAY = WM_USER + 1;
const int ID_ADD_FOREGROUND = 1001;
const int ID_TOGGLE_PULSE = 1002;
const int ID_OPEN_WHITELIST = 1003;
const int ID_EXIT = 1004;

std::set<std::wstring> g_whitelist;
std::atomic<bool> g_pulseMode(false);
std::atomic<bool> g_isPaused(false);
std::atomic<bool> g_pulseThreadRunning(false);
HANDLE g_pulseThreadHandle = NULL;
HINSTANCE g_hInstance = NULL;
HWND g_savedForeground = NULL;
// Pulse timing: wait 10 seconds between pulses, resume only for a tiny duration.
const int kPulseIntervalMs = 10000; // 10 seconds between pulses (ms)
// Duration in microseconds (allows sub-millisecond pulses). Set to 1000 = 1ms; you
// can lower to e.g. 10 for 0.01ms (10 microseconds) but values that small may
// be limited by OS scheduling and the time to perform Resume/Suspend calls.
const long long kPulseDurationUs = 1000; // 1 ms -> 1000 us

// Helper: lowercase
static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// Load whitelist into memory
void LoadWhitelist() {
    g_whitelist.clear();
    std::wifstream in(WHITELIST_FILE);
    if (!in.is_open()) return;
    std::wstring line;
    while (std::getline(in, line)) {
        // trim
        while (!line.empty() && iswspace(line.back())) line.pop_back();
        size_t i = 0; while (i < line.size() && iswspace(line[i])) ++i;
        if (i) line = line.substr(i);
        if (line.empty()) continue;
        g_whitelist.insert(ToLower(line));
    }
}

void AppendToWhitelist(const std::wstring& exeName) {
    std::wofstream out(WHITELIST_FILE, std::ios::app);
    if (!out.is_open()) return;
    out << exeName << L"\n";
    g_whitelist.insert(ToLower(exeName));
}

// Query full process image name and return file name (lowercase)
bool GetProcessExeName(DWORD pid, std::wstring& outName) {
    outName.clear();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t buf[MAX_PATH];
    DWORD size = _countof(buf);
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
        std::wstring full(buf);
        size_t pos = full.find_last_of(L"\\/");

        if (pos != std::wstring::npos) outName = full.substr(pos+1);
        else outName = full;
        outName = ToLower(outName);
        CloseHandle(h);
        return true;
    }
    CloseHandle(h);
    return false;
}

// Suspend or resume all threads of a process
bool EnumThreadsForProcess(DWORD pid, bool suspend) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (!Thread32First(snap, &te)) { CloseHandle(snap); return false; }
    do {
        if (te.th32OwnerProcessID == pid) {
            HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (ht) {
                if (suspend) SuspendThread(ht);
                else {
                    // Resume fully (call until non-positive return)
                    while (ResumeThread(ht) > 0) ;
                }
                CloseHandle(ht);
            }
        }
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return true;
}

// Pause and resume operations
void PauseProcess(DWORD pid) {
    EnumThreadsForProcess(pid, true);
}
void ResumeProcess(DWORD pid) {
    EnumThreadsForProcess(pid, false);
}

// Pulse thread: keep pulsing until stopped
DWORD WINAPI PulseWorker(LPVOID param) {
    DWORD pid = (DWORD)(ULONG_PTR)param;
    // request 1ms timer resolution so Sleep(1) is reliable for short pulses
    timeBeginPeriod(1);
    g_pulseThreadRunning = true;
    while (g_pulseThreadRunning && g_isPaused) {
        // ensure suspended
        EnumThreadsForProcess(pid, true);

        // wait for interval minus short resume duration (in small sleeps so we can exit quickly)
        int waited = 0;
        // compute millisecond portion of the configured microsecond pulse duration
        int msDurationPart = (int)(kPulseDurationUs / 1000);
        int waitTarget = (kPulseIntervalMs > msDurationPart) ? (kPulseIntervalMs - msDurationPart) : 0;
        while (g_pulseThreadRunning && g_isPaused && waited < waitTarget) {
            Sleep(50);
            waited += 50;
        }
        if (!g_pulseThreadRunning || !g_isPaused) break;

        // briefly resume to satisfy watchdog; keep duration as short as possible
        EnumThreadsForProcess(pid, false);
        // Use high-resolution busy-wait for sub-millisecond durations, otherwise Sleep
        // for the millisecond portion and busy-wait the remainder.
        if (!g_pulseThreadRunning || !g_isPaused) break;

        long long usToWait = kPulseDurationUs;
        if (usToWait >= 1000) {
            // Sleep for the whole millisecond portion first
            DWORD msPart = (DWORD)(usToWait / 1000);
            Sleep(msPart);
            usToWait -= (long long)msPart * 1000;
        }

        if (usToWait > 0) {
            // busy-wait the remaining microseconds using QueryPerformanceCounter
            LARGE_INTEGER freq, start, now;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&start);
            long long target = start.QuadPart + (freq.QuadPart * usToWait) / 1000000LL;
            do {
                QueryPerformanceCounter(&now);
            } while (g_pulseThreadRunning && g_isPaused && now.QuadPart < target);
        }
    }
    // ensure resumed when exiting
    EnumThreadsForProcess(pid, false);
    g_pulseThreadRunning = false;
    timeEndPeriod(1);
    return 0;
}

// Tray icon helper
void UpdateTrayIcon(HWND hwnd, bool paused) {
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    wcscpy_s(nid.szTip, paused ? L"GamePauser - Paused" : L"GamePauser - Running");
    Shell_NotifyIcon(paused ? NIM_MODIFY : NIM_ADD, &nid);
    // If modifying, free icon
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

// Show tray menu
void ShowTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_ADD_FOREGROUND, L"Add foreground to whitelist");
    AppendMenuW(hMenu, MF_STRING, ID_TOGGLE_PULSE, g_pulseMode ? L"Disable pulse mode" : L"Enable pulse mode");
    AppendMenuW(hMenu, MF_STRING, ID_OPEN_WHITELIST, L"Open whitelist file");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_EXIT, L"Exit");
    // save the current foreground so we can act on it (the user will right-click our tray icon)
    g_savedForeground = GetForegroundWindow();
    if (g_savedForeground == hwnd) g_savedForeground = NULL;
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);

    // clear saved foreground after menu closes
    g_savedForeground = NULL;
}

// Open whitelist in default editor
void OpenWhitelistFile() {
    std::wstring cmd = L"notepad.exe ";
    cmd += WHITELIST_FILE;
    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);
}

// Add foreground process exe to whitelist
void AddForegroundToWhitelist() {
    HWND fg = g_savedForeground ? g_savedForeground : GetForegroundWindow();
    if (!fg) return;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    if (!pid) return;
    std::wstring exe; if (!GetProcessExeName(pid, exe)) return;
    AppendToWhitelist(exe);
}

// Main window proc
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        {
            NOTIFYICONDATA nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = TRAY_ICON_ID;
            nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
            nid.uCallbackMessage = WM_TRAY;
            nid.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
            wcscpy_s(nid.szTip, L"GamePauser - Running");
            Shell_NotifyIcon(NIM_ADD, &nid);
            if (nid.hIcon) DestroyIcon(nid.hIcon);
        }
        break;
    case WM_TRAY:
        if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_ADD_FOREGROUND:
            AddForegroundToWhitelist();
            LoadWhitelist();
            break;
        case ID_TOGGLE_PULSE:
            g_pulseMode = !g_pulseMode.load();
            break;
        case ID_OPEN_WHITELIST:
            OpenWhitelistFile();
            break;
        case ID_EXIT:
            PostQuitMessage(0);
            break;
        }
        break;
    case WM_HOTKEY:
        if (wParam == 1) {
            // Get foreground process
            HWND fg = GetForegroundWindow();
            if (!fg) break;
            DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
            if (!pid) break;
            std::wstring exe; if (!GetProcessExeName(pid, exe)) break;
            if (g_whitelist.find(ToLower(exe)) == g_whitelist.end()) break; // not whitelisted

            if (!g_isPaused) {
                // Pause
                g_isPaused = true;
                if (g_pulseMode) {
                    // start pulse thread
                    if (g_pulseThreadHandle) {
                        WaitForSingleObject(g_pulseThreadHandle, INFINITE);
                        CloseHandle(g_pulseThreadHandle);
                        g_pulseThreadHandle = NULL;
                    }
                    DWORD tid;
                    g_pulseThreadHandle = CreateThread(NULL, 0, PulseWorker, (LPVOID)(ULONG_PTR)pid, 0, &tid);
                } else {
                    PauseProcess(pid);
                }
            } else {
                // Resume and stop pulse
                g_isPaused = false;
                g_pulseThreadRunning = false;
                if (g_pulseThreadHandle) {
                    WaitForSingleObject(g_pulseThreadHandle, INFINITE);
                    CloseHandle(g_pulseThreadHandle);
                    g_pulseThreadHandle = NULL;
                }
                ResumeProcess(pid);
            }
            UpdateTrayIcon(hwnd, g_isPaused.load());
        }
        break;
    case WM_DESTROY:
        {
            NOTIFYICONDATA nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = TRAY_ICON_ID;
            Shell_NotifyIcon(NIM_DELETE, &nid);
            PostQuitMessage(0);
        }
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hInstance = hInstance;
    LoadWhitelist();

    const wchar_t CLASS_NAME[] = L"GamePauserWndClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASS_NAME, L"GamePauser", 0, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);
    if (!hwnd) return 0;

    // register global hotkey: Pause
    RegisterHotKey(hwnd, 1, 0, VK_PAUSE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // cleanup
    g_pulseThreadRunning = false;
    g_isPaused = false;
    if (g_pulseThreadHandle) {
        WaitForSingleObject(g_pulseThreadHandle, INFINITE);
        CloseHandle(g_pulseThreadHandle);
        g_pulseThreadHandle = NULL;
    }
    UnregisterHotKey(hwnd, 1);
    return 0;
}

// When the project is built as a console application the CRT expects a main();
// provide one that forwards to wWinMain so the linker is satisfied.
int main() {
    return (int)wWinMain(GetModuleHandle(NULL), NULL, GetCommandLineW(), SW_SHOWDEFAULT);
}
