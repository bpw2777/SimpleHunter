// Source Code
#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup")

// ID elements for controlling
#define ID_EDIT_INPUT       101
#define ID_BTN_SEARCH       102
#define ID_LIST_RESULTS     103
#define ID_COMBO_EXT        104
#define ID_COMBO_TARGET     105
#define ID_BTN_STARTUP      106
#define ID_BTN_BACK         107
#define ID_BTN_FORWARD      108
#define ID_PROGRESS_BAR     109

#define ID_MENU_OPEN        201
#define ID_MENU_COPY        202
#define ID_MENU_DELETE      203
#define ID_MENU_COPY_ALL    204
#define ID_MENU_DELETE_ALL  205

#define WM_SEARCH_BATCH     (WM_APP + 1)
#define WM_SEARCH_FINISHED  (WM_APP + 2)

// Global variables Interface
HWND g_hwnd = nullptr;
HWND g_edit = nullptr;
HWND g_list = nullptr;
HWND g_combo = nullptr;
HWND g_comboTarget = nullptr;
HWND g_startBtn = nullptr;
HWND g_searchBtn = nullptr;
HWND g_backBtn = nullptr;
HWND g_forwardBtn = nullptr;
HWND g_progressBar = nullptr;
HFONT g_font = nullptr;
WNDPROC g_originalEditProc = nullptr;

// Setting search
std::atomic<bool> g_cancelSearch{ false };
std::thread g_workerThread;
std::vector<std::wstring> g_resultsCache;
bool g_searchRegistry = false; // false = Disk, true = Registry
bool g_isStartupView = false;

// History state navigation
enum class AppView {
    Results,
    Startup
};

AppView g_currentView = AppView::Results;
AppView g_historyLeft = AppView::Results;
AppView g_historyRight = AppView::Results;

void UpdateNavigationState(AppView newView) {
    if (g_currentView != newView) {
        g_historyLeft = g_currentView;
        g_historyRight = newView;
        g_currentView = newView;
    }
}

// Subclass procedure for Edit control to support Ctrl + A, Ctrl + Z and prevent system beep
LRESULT CALLBACK SubclassedEditProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    if (uMsg == WM_KEYDOWN && wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(hwnd, EM_UNDO, 0, 0);
        return 0;
    }
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        SendMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(ID_BTN_SEARCH, BN_CLICKED), (LPARAM)g_searchBtn);
        return 0;
    }
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        return 0;
    }
    if (uMsg == WM_CHAR && (wParam == 1 || wParam == 0x01) && (GetKeyState(VK_CONTROL) & 0x8000)) {
        return 0;
    }
    if (uMsg == WM_CHAR && (wParam == 26 || wParam == 0x1A) && (GetKeyState(VK_CONTROL) & 0x8000)) {
        return 0;
    }
    return CallWindowProcW(g_originalEditProc, hwnd, uMsg, wParam, lParam);
}

// Expanded Extension Checking
bool CheckExtension(const std::wstring& fileName, int filterIndex) {
    if (filterIndex == 0) return true; // All

    size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;

    std::wstring ext = fileName.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));

    switch (filterIndex) {
    case 1: // Archives
        return (ext == L".zip" || ext == L".rar" || ext == L".7z" || ext == L".gz");
    case 2: // Programs
        return (ext == L".exe" || ext == L".bat" || ext == L".ps1");
    case 3: // Coding
        return (ext == L".cpp" || ext == L".cs" || ext == L".html" || ext == L".css" ||
            ext == L".js" || ext == L".java" || ext == L".py" || ext == L".asm" ||
            ext == L".c" || ext == L".dll" || ext == L".lua" || ext == L".h" ||
            ext == L".ts" || ext == L".php" || ext == L".rs" || ext == L".kt" ||
            ext == L".go" || ext == L".sh");
    case 4: // Documents
        return (ext == L".txt" || ext == L".doc" || ext == L".xls" || ext == L".ppt" ||
            ext == L".docm" || ext == L".pdf" || ext == L".odt" || ext == L".rtf" ||
            ext == L".xlsm" || ext == L".pptm");
    default: return true;
    }
}

// Improved System Path Protection
bool IsProtectedPath(const std::wstring& path) {
    std::wstring lowPath = path;
    for (auto& c : lowPath) c = static_cast<wchar_t>(towlower(c));

    if (lowPath.find(L"\\windows\\") != std::wstring::npos ||
        lowPath.find(L"\\program files\\") != std::wstring::npos ||
        lowPath.find(L"\\program files (x86)\\") != std::wstring::npos ||
        lowPath.find(L"\\programdata\\") != std::wstring::npos ||
        lowPath.find(L"\\system volume information") != std::wstring::npos ||
        lowPath.find(L"\\$recycle.bin") != std::wstring::npos) {
        return true;
    }
    return false;
}

// Check if file is currently running / locked by another process
bool IsFileInUse(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_ACCESS_DENIED) {
            return true;
        }
    }
    else {
        CloseHandle(hFile);
    }
    return false;
}

// Recursive file search with batching support
void SearchFilesRecursive(HWND hwnd, const std::wstring& directory, const std::wstring& target, int filterIndex, std::vector<std::wstring>& batchBuffer) {
    if (g_cancelSearch.load()) return;
    if (IsProtectedPath(directory)) return;

    std::wstring query = directory + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(query.c_str(), &fd);

    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (g_cancelSearch.load()) break;

        std::wstring fn = fd.cFileName;
        if (fn == L".." || fn == L".") continue;

        std::wstring fp = directory + L"\\" + fn;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                SearchFilesRecursive(hwnd, fp, target, filterIndex, batchBuffer);
            }
        }
        else {
            std::wstring lowFn = fn;
            for (auto& c : lowFn) c = static_cast<wchar_t>(towlower(c));

            if (lowFn.find(target) != std::wstring::npos) {
                if (CheckExtension(fn, filterIndex)) {
                    batchBuffer.push_back(fp);
                    if (batchBuffer.size() >= 50) {
                        auto* pBatch = new std::vector<std::wstring>(batchBuffer);
                        PostMessageW(hwnd, WM_SEARCH_BATCH, 0, reinterpret_cast<LPARAM>(pBatch));
                        batchBuffer.clear();
                    }
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void SearchAllDisks(HWND hwnd, const std::wstring& target, int filterIndex) {
    wchar_t drives[256];
    DWORD len = GetLogicalDriveStringsW(256, drives);
    if (len == 0) return;

    std::vector<std::wstring> batchBuffer;
    wchar_t* pDrive = drives;
    while (*pDrive && !g_cancelSearch.load()) {
        UINT type = GetDriveTypeW(pDrive);
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
            std::wstring root = pDrive;
            if (!root.empty() && root.back() == L'\\') {
                root.pop_back();
            }
            SearchFilesRecursive(hwnd, root, target, filterIndex, batchBuffer);
        }
        pDrive += wcslen(pDrive) + 1;
    }

    if (!batchBuffer.empty() && !g_cancelSearch.load()) {
        auto* pBatch = new std::vector<std::wstring>(batchBuffer);
        PostMessageW(hwnd, WM_SEARCH_BATCH, 0, reinterpret_cast<LPARAM>(pBatch));
    }
}

// Registry Search with batching
void SearchRegistryRecursive(HWND hwnd, HKEY hKey, const std::wstring& keyPath, const std::wstring& target, std::vector<std::wstring>& batchBuffer) {
    if (g_cancelSearch.load()) return;

    DWORD subKeyCount = 0, maxSubKeyLen = 0, valueCount = 0, maxValueNameLen = 0, maxValueDataLen = 0;
    if (RegQueryInfoKeyW(hKey, NULL, NULL, NULL, &subKeyCount, &maxSubKeyLen, NULL, &valueCount, &maxValueNameLen, &maxValueDataLen, NULL, NULL) == ERROR_SUCCESS) {

        std::vector<wchar_t> valName(maxValueNameLen + 1);
        // +1 extra wchar_t of headroom in case registry data isn't NUL-terminated
        std::vector<BYTE> valData(maxValueDataLen + sizeof(wchar_t));

        for (DWORD i = 0; i < valueCount; ++i) {
            if (g_cancelSearch.load()) return;
            DWORD nameLen = (DWORD)valName.size();
            DWORD dataLen = (DWORD)(valData.size() - sizeof(wchar_t));
            DWORD type = 0;

            if (RegEnumValueW(hKey, i, valName.data(), &nameLen, NULL, &type, valData.data(), &dataLen) == ERROR_SUCCESS) {
                std::wstring name(valName.data(), nameLen);
                std::wstring dataStr = L"";
                if (type == REG_SZ || type == REG_EXPAND_SZ) {
                    // Registry string data is not guaranteed to be NUL-terminated.
                    // Build the string from the exact byte count instead of trusting a terminator.
                    size_t charCount = dataLen / sizeof(wchar_t);
                    const wchar_t* raw = reinterpret_cast<const wchar_t*>(valData.data());
                    dataStr.assign(raw, charCount);
                    // Trim a trailing NUL if present so string length/searches stay clean
                    while (!dataStr.empty() && dataStr.back() == L'\0') {
                        dataStr.pop_back();
                    }
                }

                std::wstring lowName = name;
                std::wstring lowData = dataStr;
                for (auto& c : lowName) c = towlower(c);
                for (auto& c : lowData) c = towlower(c);

                if (lowName.find(target) != std::wstring::npos || lowData.find(target) != std::wstring::npos) {
                    std::wstring result = keyPath + L"\\" + name + L" = " + dataStr;
                    batchBuffer.push_back(result);
                    if (batchBuffer.size() >= 50) {
                        auto* pBatch = new std::vector<std::wstring>(batchBuffer);
                        PostMessageW(hwnd, WM_SEARCH_BATCH, 0, reinterpret_cast<LPARAM>(pBatch));
                        batchBuffer.clear();
                    }
                }
            }
        }

        std::vector<wchar_t> subKeyName(maxSubKeyLen + 1);
        for (DWORD i = 0; i < subKeyCount; ++i) {
            if (g_cancelSearch.load()) return;
            DWORD nameLen = (DWORD)subKeyName.size();
            if (RegEnumKeyExW(hKey, i, subKeyName.data(), &nameLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                std::wstring subName(subKeyName.data(), nameLen);
                HKEY hSubKey = NULL;
                if (RegOpenKeyExW(hKey, subName.c_str(), 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
                    SearchRegistryRecursive(hwnd, hSubKey, keyPath + L"\\" + subName, target, batchBuffer);
                    RegCloseKey(hSubKey);
                }
            }
        }
    }
}

void SearchRegistry(HWND hwnd, const std::wstring& target) {
    HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    const wchar_t* rootNames[] = { L"HKEY_CURRENT_USER", L"HKEY_LOCAL_MACHINE" };
    std::vector<std::wstring> batchBuffer;

    for (int i = 0; i < 2; ++i) {
        if (g_cancelSearch.load()) return;
        HKEY hKey = NULL;
        if (RegOpenKeyExW(roots[i], L"", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            SearchRegistryRecursive(hwnd, hKey, rootNames[i], target, batchBuffer);
            RegCloseKey(hKey);
        }
    }

    if (!batchBuffer.empty() && !g_cancelSearch.load()) {
        auto* pBatch = new std::vector<std::wstring>(batchBuffer);
        PostMessageW(hwnd, WM_SEARCH_BATCH, 0, reinterpret_cast<LPARAM>(pBatch));
    }
}

void AddItemToList(HWND listview, const std::wstring& text) {
    LVITEMW lvi = { 0 };
    lvi.mask = LVIF_TEXT;
    lvi.iItem = ListView_GetItemCount(listview);
    lvi.pszText = const_cast<LPWSTR>(text.c_str());
    ListView_InsertItem(listview, &lvi);
}

void ShowStartupItems(HWND listview) {
    g_isStartupView = true;
    UpdateNavigationState(AppView::Startup);
    ListView_DeleteAllItems(listview);
    g_resultsCache.clear();

    HKEY hKey;
    LPCWSTR runs[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"
    };
    HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };

    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 2; i++) {
            if (RegOpenKeyExW(roots[r], runs[i], 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                wchar_t name[256], data[MAX_PATH];
                DWORD nSize, dSize, type;
                for (int j = 0; ; j++) {
                    nSize = 256; dSize = MAX_PATH;
                    if (RegEnumValueW(hKey, j, name, &nSize, NULL, &type, (LPBYTE)data, &dSize) != ERROR_SUCCESS) break;

                    std::wstring entry = (roots[r] == HKEY_LOCAL_MACHINE ? L"[System] " : L"[User] ")
                        + std::wstring(name) + L" -> " + data;

                    AddItemToList(listview, entry);
                    g_resultsCache.push_back(entry);
                }
                RegCloseKey(hKey);
            }
        }
    }
}

void CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (hMem) {
        memcpy(GlobalLock(hMem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
}

int GetSelectedListViewIndex(HWND listview) {
    return ListView_GetNextItem(listview, -1, LVNI_SELECTED);
}

// Returns indices of all selected items (listview must have LVS_SHOWSELALWAYS; selection
// is multi-select because we removed LVS_SINGLESEL and handle Ctrl+click natively via the
// control's own multi-select behavior).
std::vector<int> GetSelectedListViewIndices(HWND listview) {
    std::vector<int> result;
    int idx = -1;
    while ((idx = ListView_GetNextItem(listview, idx, LVNI_SELECTED)) != -1) {
        result.push_back(idx);
    }
    return result;
}

void ShowContextMenu(HWND hwnd, LPARAM lParam) {
    POINT pt = { LOWORD(lParam), HIWORD(lParam) };
    std::vector<int> sel = GetSelectedListViewIndices(g_list);

    if (!sel.empty()) {
        HMENU hMenu = CreatePopupMenu();

        if (sel.size() == 1) {
            if (!g_isStartupView && !g_searchRegistry) {
                AppendMenuW(hMenu, MF_STRING, ID_MENU_OPEN, L"Locate in Explorer");
            }
            AppendMenuW(hMenu, MF_STRING, ID_MENU_COPY, L"Copy Path");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_MENU_DELETE, L"Delete");
        }
        else {
            // Multiple items selected: "Locate in Explorer" is hidden (ambiguous for >1 item),
            // Copy/Delete operate on the whole selection.
            AppendMenuW(hMenu, MF_STRING, ID_MENU_COPY_ALL, L"Copy All Path");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_MENU_DELETE_ALL, L"Delete All");
        }

        TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hMenu);
    }
}

void LayoutControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    MoveWindow(g_backBtn, 10, 15, 28, 25, TRUE);
    MoveWindow(g_forwardBtn, 42, 15, 28, 25, TRUE);
    MoveWindow(g_edit, 75, 15, w - 460, 25, TRUE);
    MoveWindow(g_comboTarget, w - 380, 15, 110, 300, TRUE);
    MoveWindow(g_combo, w - 265, 15, 110, 300, TRUE);
    MoveWindow(g_searchBtn, w - 150, 15, 140, 25, TRUE);

    MoveWindow(g_list, 10, 50, w - 20, h - 95, TRUE);
    MoveWindow(g_progressBar, 10, h - 40, w - 20, 4, TRUE);
    MoveWindow(g_startBtn, 10, h - 32, w - 20, 25, TRUE);

    // Set column width reasonably wide to allow horizontal scrolling without excess
    ListView_SetColumnWidth(g_list, 0, 1200);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        SetProcessDPIAware();
        INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icex);

        g_font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_backBtn = CreateWindowExW(0, L"BUTTON", L"<", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 10, 15, 28, 25, hwnd, (HMENU)ID_BTN_BACK, NULL, NULL);
        g_forwardBtn = CreateWindowExW(0, L"BUTTON", L">", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 42, 15, 28, 25, hwnd, (HMENU)ID_BTN_FORWARD, NULL, NULL);

        g_edit = CreateWindowExW(0, L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 75, 15, 160, 25, hwnd, (HMENU)ID_EDIT_INPUT, NULL, NULL);

        g_originalEditProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)SubclassedEditProc);

        g_comboTarget = CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 240, 15, 110, 300, hwnd, (HMENU)ID_COMBO_TARGET, NULL, NULL);
        SendMessageW(g_comboTarget, CB_ADDSTRING, 0, (LPARAM)L"Disk");
        SendMessageW(g_comboTarget, CB_ADDSTRING, 0, (LPARAM)L"Registry");
        SendMessageW(g_comboTarget, CB_SETCURSEL, 0, 0);

        g_combo = CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, 355, 15, 110, 300, hwnd, (HMENU)ID_COMBO_EXT, NULL, NULL);

        const wchar_t* filters[] = {
            L"All",
            L"Archives",
            L"Programs",
            L"Coding",
            L"Documents"
        };
        for (int i = 0; i < 5; i++) SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)filters[i]);
        SendMessageW(g_combo, CB_SETCURSEL, 0, 0);

        g_searchBtn = CreateWindowExW(0, L"BUTTON", L"Search Now", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 470, 15, 140, 25, hwnd, (HMENU)ID_BTN_SEARCH, NULL, NULL);

        // LVS_SINGLESEL removed so Ctrl+click / Shift+click multi-selection works (standard
        // ListView behavior once single-select is not forced).
        g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
            10, 50, 600, 300, hwnd, (HMENU)ID_LIST_RESULTS, NULL, NULL);

        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.pszText = (LPWSTR)L"Results";
        lvc.cx = 800;
        ListView_InsertColumn(g_list, 0, &lvc);

        g_progressBar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD, 10, 360, 600, 4, hwnd, (HMENU)ID_PROGRESS_BAR, NULL, NULL);
        SendMessageW(g_progressBar, PBM_SETMARQUEE, (WPARAM)TRUE, 50);

        g_startBtn = CreateWindowExW(0, L"BUTTON", L"Show Startup Items", WS_VISIBLE | WS_CHILD, 10, 368, 600, 25, hwnd, (HMENU)ID_BTN_STARTUP, NULL, NULL);

        EnumChildWindows(hwnd, [](HWND child, LPARAM font) -> BOOL {
            SendMessage(child, WM_SETFONT, font, TRUE);
            return TRUE;
            }, (LPARAM)g_font);
        break;
    }
    case WM_SIZE:
        LayoutControls(hwnd);
        break;

    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == g_list) {
            ShowContextMenu(hwnd, lParam);
        }
        break;

    case WM_SEARCH_BATCH: {
        auto* batch = reinterpret_cast<std::vector<std::wstring>*>(lParam);
        if (batch) {
            for (const auto& path : *batch) {
                AddItemToList(g_list, path);
                g_resultsCache.push_back(path);
            }
            delete batch;

            wchar_t title[64];
            swprintf_s(title, L"SimpleHunter \u2014 %zu found", g_resultsCache.size());
            SetWindowTextW(g_hwnd, title);
        }
        break;
    }
    case WM_SEARCH_FINISHED:
        if (g_workerThread.joinable()) g_workerThread.join();
        ShowWindow(g_progressBar, SW_HIDE);
        EnableWindow(g_startBtn, TRUE);
        EnableWindow(g_searchBtn, TRUE);
        break;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int notification = HIWORD(wParam);

        if (id == ID_COMBO_TARGET && notification == CBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(g_comboTarget, CB_GETCURSEL, 0, 0));
            g_searchRegistry = (sel == 1);
        }

        if (id == ID_BTN_SEARCH) {
            wchar_t b[256];
            GetWindowTextW(g_edit, b, 256);
            if (wcslen(b) < 2) {
                MessageBoxW(hwnd, L"Enter at least 2 characters!", L"Error", MB_ICONWARNING);
                break;
            }

            g_isStartupView = false;
            UpdateNavigationState(AppView::Results);
            ListView_DeleteAllItems(g_list);
            g_resultsCache.clear();

            std::wstring query = b;
            for (auto& c : query) c = static_cast<wchar_t>(towlower(c));
            int filterIdx = static_cast<int>(SendMessageW(g_combo, CB_GETCURSEL, 0, 0));
            bool isReg = g_searchRegistry;

            if (g_workerThread.joinable()) g_workerThread.join();
            g_cancelSearch = false;

            // Disable controls that touch g_resultsCache/g_list while the worker thread is
            // writing to them via PostMessage, to avoid a data race with Delete/Startup actions.
            EnableWindow(g_startBtn, FALSE);
            EnableWindow(g_searchBtn, FALSE);

            ShowWindow(g_progressBar, SW_SHOW);
            SendMessageW(g_progressBar, PBM_SETBARCOLOR, 0, (LPARAM)RGB(0, 200, 0));
            SetWindowLongPtrW(g_progressBar, GWL_STYLE, GetWindowLongPtrW(g_progressBar, GWL_STYLE) | PBS_MARQUEE);
            SendMessageW(g_progressBar, PBM_SETMARQUEE, (WPARAM)TRUE, 30);

            HWND hMainWnd = hwnd;
            g_workerThread = std::thread([hMainWnd, query, filterIdx, isReg]() {
                if (isReg) {
                    SearchRegistry(hMainWnd, query);
                }
                else {
                    SearchAllDisks(hMainWnd, query, filterIdx);
                }
                PostMessageW(hMainWnd, WM_SEARCH_FINISHED, 0, 0);
                });
        }
        else if (id == ID_BTN_STARTUP) {
            ShowStartupItems(g_list);
        }
        else if (id == ID_BTN_BACK) {
            if (g_currentView != g_historyLeft) {
                g_currentView = g_historyLeft;
                if (g_currentView == AppView::Startup) {
                    ShowStartupItems(g_list);
                }
                else {
                    g_isStartupView = false;
                    ListView_DeleteAllItems(g_list);
                }
            }
        }
        else if (id == ID_BTN_FORWARD) {
            if (g_currentView != g_historyRight) {
                g_currentView = g_historyRight;
                if (g_currentView == AppView::Startup) {
                    ShowStartupItems(g_list);
                }
                else {
                    g_isStartupView = false;
                    ListView_DeleteAllItems(g_list);
                }
            }
        }
        else if (id == ID_MENU_OPEN) {
            int sel = GetSelectedListViewIndex(g_list);
            if (sel != -1 && sel < static_cast<int>(g_resultsCache.size())) {
                std::wstring cmd = L"/select,\"" + g_resultsCache[sel] + L"\"";
                ShellExecuteW(NULL, L"open", L"explorer.exe", cmd.c_str(), NULL, SW_SHOWNORMAL);
            }
        }
        else if (id == ID_MENU_COPY) {
            int sel = GetSelectedListViewIndex(g_list);
            if (sel != -1 && sel < static_cast<int>(g_resultsCache.size())) {
                CopyToClipboard(g_resultsCache[sel]);
            }
        }
        else if (id == ID_MENU_COPY_ALL) {
            std::vector<int> sel = GetSelectedListViewIndices(g_list);
            std::wstring joined;
            for (int idx : sel) {
                if (idx >= 0 && idx < static_cast<int>(g_resultsCache.size())) {
                    if (!joined.empty()) joined += L"\r\n";
                    joined += g_resultsCache[idx];
                }
            }
            if (!joined.empty()) CopyToClipboard(joined);
        }
        else if (id == ID_MENU_DELETE || id == ID_MENU_DELETE_ALL) {
            std::vector<int> sel = (id == ID_MENU_DELETE)
                ? std::vector<int>{ GetSelectedListViewIndex(g_list) }
            : GetSelectedListViewIndices(g_list);

            // Remove any invalid index (e.g. -1 from a single-select call with nothing selected)
            sel.erase(std::remove_if(sel.begin(), sel.end(), [](int i) {
                return i < 0 || i >= static_cast<int>(g_resultsCache.size());
                }), sel.end());

            if (sel.empty()) break;

            // Sort descending so we can erase from g_resultsCache / ListView by index safely
            // without indices shifting under us as we go.
            std::sort(sel.begin(), sel.end(), std::greater<int>());

            if (!g_isStartupView) {
                // Pre-check: any selected file currently in use?
                for (int idx : sel) {
                    if (IsFileInUse(g_resultsCache[idx])) {
                        MessageBoxW(hwnd, L"Cannot delete: one or more selected files are currently running or in use by another process!", L"Error: File Running", MB_OK | MB_ICONERROR);
                        goto delete_done;
                    }
                }
            }

            {
                wchar_t confirmMsg[128];
                if (sel.size() == 1) {
                    wcscpy_s(confirmMsg, L"Are you sure you want to delete the selected item?");
                }
                else {
                    swprintf_s(confirmMsg, L"Are you sure you want to delete the selected %zu items?", sel.size());
                }
                if (MessageBoxW(hwnd, confirmMsg, L"Confirm Action", MB_YESNO | MB_ICONWARNING) != IDYES) {
                    break;
                }
            }

            {
                int successCount = 0;
                int failCount = 0;

                for (int idx : sel) {
                    std::wstring targetPath = g_resultsCache[idx];

                    if (g_isStartupView) {
                        std::wstring entry = targetPath;
                        bool isSystem = (entry.find(L"[System]") == 0);

                        size_t nameStart = entry.find(L"] ") + 2;
                        size_t arrowPos = entry.find(L" -> ");
                        if (nameStart != std::wstring::npos && arrowPos != std::wstring::npos) {
                            std::wstring valName = entry.substr(nameStart, arrowPos - nameStart);
                            HKEY rootKey = isSystem ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

                            bool removed = false;
                            LPCWSTR runPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
                            HKEY hKey = NULL;
                            if (RegOpenKeyExW(rootKey, runPath, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
                                if (RegDeleteValueW(hKey, valName.c_str()) == ERROR_SUCCESS) removed = true;
                                RegCloseKey(hKey);
                            }

                            LPCWSTR runOncePath = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
                            if (RegOpenKeyExW(rootKey, runOncePath, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
                                if (RegDeleteValueW(hKey, valName.c_str()) == ERROR_SUCCESS) removed = true;
                                RegCloseKey(hKey);
                            }

                            if (removed) {
                                ListView_DeleteItem(g_list, idx);
                                g_resultsCache.erase(g_resultsCache.begin() + idx);
                                successCount++;
                            }
                            else {
                                failCount++;
                            }
                        }
                        else {
                            failCount++;
                        }
                    }
                    else {
                        std::wstring lowTarget = targetPath;
                        for (auto& c : lowTarget) c = static_cast<wchar_t>(towlower(c));

                        if (lowTarget.find(L"\\windows") != std::wstring::npos ||
                            lowTarget.find(L"\\system32") != std::wstring::npos ||
                            lowTarget.find(L"\\boot") != std::wstring::npos ||
                            lowTarget.length() <= 4) {
                            failCount++;
                            continue;
                        }

                        targetPath.push_back(L'\0');

                        SHFILEOPSTRUCTW fileOp = { 0 };
                        fileOp.hwnd = hwnd;
                        fileOp.wFunc = FO_DELETE;
                        fileOp.pFrom = targetPath.c_str();
                        fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI;

                        int res = SHFileOperationW(&fileOp);
                        if (res == 0 && !fileOp.fAnyOperationsAborted) {
                            ListView_DeleteItem(g_list, idx);
                            g_resultsCache.erase(g_resultsCache.begin() + idx);
                            successCount++;
                        }
                        else {
                            failCount++;
                        }
                    }
                }

                if (g_isStartupView) {
                    if (failCount == 0) {
                        MessageBoxW(hwnd, successCount == 1 ? L"Startup item successfully removed." : L"Startup items successfully removed.", L"Success", MB_OK);
                    }
                    else if (successCount > 0) {
                        wchar_t msg[128];
                        swprintf_s(msg, L"%d removed, %d failed.", successCount, failCount);
                        MessageBoxW(hwnd, msg, L"Partial Success", MB_OK | MB_ICONWARNING);
                    }
                    else {
                        MessageBoxW(hwnd, L"Failed to remove startup entry.", L"Error", MB_ICONERROR);
                    }
                }
                else {
                    if (failCount == 0) {
                        MessageBoxW(hwnd, successCount == 1 ? L"File moved to Recycle Bin." : L"Files moved to Recycle Bin.", L"Success", MB_OK);
                    }
                    else if (successCount > 0) {
                        wchar_t msg[160];
                        swprintf_s(msg, L"%d moved to Recycle Bin, %d failed or were protected.", successCount, failCount);
                        MessageBoxW(hwnd, msg, L"Partial Success", MB_OK | MB_ICONWARNING);
                    }
                    else {
                        MessageBoxW(hwnd, L"Failed to move file(s) to Recycle Bin.", L"Error", MB_ICONERROR);
                    }
                }
            }
        delete_done:;
        }
        break;
    }
    case WM_DESTROY:
        g_cancelSearch = true;
        if (g_workerThread.joinable()) g_workerThread.join();
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_hwnd = NULL;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"FileHunterClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, L"FileHunterClass", L"SimpleHunter",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 635, 440,
        NULL, NULL, hInst, NULL
    );

    if (!hwnd) return 0;
    g_hwnd = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return static_cast<int>(m.wParam);
}
