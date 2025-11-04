#define UNICODE
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <ole2.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <locale>
#include <codecvt>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

// Resource IDs
#define IDI_SEEKER 100
#define IDD_INSTALLER 101
#define IDC_UNICODE 1001
#define IDC_ANSI 1002
#define IDC_STARTMENU 1003
#define IDC_DESKTOP 1004
#define IDC_INSTALL_BTN 1005
#define IDC_CANCEL_BTN 1006

// Virtual key codes
#define VK_K 0x4B

// Global variables
std::map<int, std::vector<std::string>> keyCommands;
HHOOK keyboardHook = NULL;
NOTIFYICONDATA nid = {0};
HWND hwnd = NULL;
std::atomic<bool> running(true);
std::atomic<bool> isPaused(false);
std::thread scriptThread;
HINSTANCE hInstance;

// Forward declaration
void executeScriptWithBindings(const std::wstring& filename, std::set<std::wstring>& includedFiles);

// Function to create shortcuts
BOOL CreateShortcut(LPCWSTR targetPath, LPCWSTR shortcutPath, LPCWSTR description) {
    CoInitialize(NULL);
    
    IShellLinkW* pShellLink = NULL;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID*)&pShellLink);
    if (SUCCEEDED(hr)) {
        pShellLink->SetPath(targetPath);
        pShellLink->SetDescription(description);
        
        IPersistFile* pPersistFile = NULL;
        hr = pShellLink->QueryInterface(IID_IPersistFile, (LPVOID*)&pPersistFile);
        if (SUCCEEDED(hr)) {
            hr = pPersistFile->Save(shortcutPath, TRUE);
            pPersistFile->Release();
        }
        pShellLink->Release();
    }
    
    CoUninitialize();
    return SUCCEEDED(hr);
}

// Function to uninstall the application
void uninstall() {
    HKEY hKey;

    // Remove .sekeer extension association
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L".sekeer");

    // Remove ProgID
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"SeekerScript");

    // Remove shortcuts
    WCHAR startMenuPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, startMenuPath))) {
        std::wstring shortcutPath = std::wstring(startMenuPath) + L"\\Seeker Script.lnk";
        DeleteFileW(shortcutPath.c_str());
    }

    WCHAR desktopPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOP, NULL, 0, desktopPath))) {
        std::wstring shortcutPath = std::wstring(desktopPath) + L"\\Seeker Script.lnk";
        DeleteFileW(shortcutPath.c_str());
    }
}

// Function to associate .sekeer extension with the executable
void install(bool createStartMenu, bool createDesktop) {
    HKEY hKey;
    WCHAR buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring exePathW = buffer;

    // Associate .sekeer extension
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, L".sekeer", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring progId = L"SeekerScript";
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)progId.c_str(), (progId.size() + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }

    // Create ProgID
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, L"SeekerScript", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring desc = L"Seeker Script File";
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)desc.c_str(), (desc.size() + 1) * sizeof(WCHAR));

        // Set default icon
        HKEY hIconKey;
        if (RegCreateKeyExW(hKey, L"DefaultIcon", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hIconKey, NULL) == ERROR_SUCCESS) {
            std::wstring iconPath = exePathW + L",0";
            RegSetValueExW(hIconKey, NULL, 0, REG_SZ, (BYTE*)iconPath.c_str(), (iconPath.size() + 1) * sizeof(WCHAR));
            RegCloseKey(hIconKey);
        }

        HKEY hSubKey;
        if (RegCreateKeyExW(hKey, L"shell\\open\\command", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hSubKey, NULL) == ERROR_SUCCESS) {
            std::wstring cmd = L"\"" + exePathW + L"\" \"%1\"";
            RegSetValueExW(hSubKey, NULL, 0, REG_SZ, (BYTE*)cmd.c_str(), (cmd.size() + 1) * sizeof(WCHAR));
            RegCloseKey(hSubKey);
        }
        RegCloseKey(hKey);
    }

    // Create shortcuts
    if (createStartMenu) {
        WCHAR startMenuPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, startMenuPath))) {
            std::wstring shortcutPath = std::wstring(startMenuPath) + L"\\Seeker Script.lnk";
            CreateShortcut(exePathW.c_str(), shortcutPath.c_str(), L"Seeker Script Automation");
        }
    }

    if (createDesktop) {
        WCHAR desktopPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOP, NULL, 0, desktopPath))) {
            std::wstring shortcutPath = std::wstring(desktopPath) + L"\\Seeker Script.lnk";
            CreateShortcut(exePathW.c_str(), shortcutPath.c_str(), L"Seeker Script Automation");
        }
    }
}

// Optimized function to send keystrokes without hook interference
void sendKeys(const std::string& text) {
    static bool sendingKeys = false;
    if (sendingKeys) return;

    sendingKeys = true;

    // Temporarily disable the keyboard hook to prevent recursion
    HHOOK tempHook = keyboardHook;
    keyboardHook = NULL;

    std::vector<INPUT> inputs;
    inputs.reserve(text.length() * 2);

    for (char c : text) {
        // Use Unicode input to work with any keyboard layout
        INPUT inputDown = {0};
        inputDown.type = INPUT_KEYBOARD;
        inputDown.ki.wScan = c;  // Unicode character
        inputDown.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(inputDown);

        // Key up
        INPUT inputUp = {0};
        inputUp.type = INPUT_KEYBOARD;
        inputUp.ki.wScan = c;  // Unicode character
        inputUp.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(inputUp);
    }

    if (!inputs.empty()) {
        SendInput(inputs.size(), inputs.data(), sizeof(INPUT));
    }

    // Restore the hook
    keyboardHook = tempHook;
    sendingKeys = false;
}

// Function to move mouse
void moveMouse(int x, int y, bool absolute = false) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    
    if (absolute) {
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        input.mi.dx = (x * 65535) / screenWidth;
        input.mi.dy = (y * 65535) / screenHeight;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    } else {
        input.mi.dx = x;
        input.mi.dy = y;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
    }
    
    SendInput(1, &input, sizeof(INPUT));
}

// Function to click mouse
void clickMouse(const std::string& button = "left", bool down = true) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    
    if (button == "left") {
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    } else if (button == "right") {
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else if (button == "middle") {
        input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }
    
    SendInput(1, &input, sizeof(INPUT));
}

// Function to execute commands in a loop
void executeLoop(const std::vector<std::string>& commands, int iterations) {
    for (int i = 0; i < iterations && running; i++) {
        while (isPaused && running) {
            Sleep(100); // Wait while paused
        }
        for (const auto& cmd : commands) {
            if (!running) break;
            while (isPaused && running) {
                Sleep(100); // Wait while paused
            }

            std::istringstream iss(cmd);
            std::string command;
            iss >> command;

            if (command == "otpravit") {
                std::string text;
                std::getline(iss, text);
                text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](int ch) { return !std::isspace(ch); }));
                sendKeys(text);
            } else if (command == "спящий") {
                int ms;
                iss >> ms;
                Sleep(ms);
            } else if (command == "мышь") {
                int x, y;
                iss >> x >> y;
                moveMouse(x, y, false);
            } else if (command == "абсолютная_мышь") {
                int x, y;
                iss >> x >> y;
                moveMouse(x, y, true);
            } else if (command == "клик") {
                std::string button;
                iss >> button;
                clickMouse(button, true);
                Sleep(10);
                clickMouse(button, false);
            } else if (command == "otpravit_txt") {
                std::string filename;
                iss >> filename;
                std::ifstream txtFile(filename);
                std::string line;
                while (std::getline(txtFile, line) && running) {
                    while (isPaused && running) {
                        Sleep(100); // Wait while paused
                    }
                    sendKeys(line);
                    Sleep(100); // Small delay between lines
                }
            }
        }
    }
}

// Keyboard hook procedure - optimized
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        auto it = keyCommands.find(p->vkCode);
        if (it != keyCommands.end()) {
            std::thread([it]() {
                for (const auto& cmd : it->second) {
                    if (!running) break;
                    while (isPaused && running) {
                        Sleep(100); // Wait while paused
                    }

                    std::istringstream iss(cmd);
                    std::string command;
                    iss >> command;

                    if (command == "otpravit") {
                        std::string text;
                        std::getline(iss, text);
                        text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](int ch) { return !std::isspace(ch); }));
                        sendKeys(text);
                    } else if (command == "спящий") {
                        int ms;
                        iss >> ms;
                        Sleep(ms);
                    } else if (command == "мышь") {
                        int x, y;
                        iss >> x >> y;
                        moveMouse(x, y, false);
                    } else if (command == "абсолютная_мышь") {
                        int x, y;
                        iss >> x >> y;
                        moveMouse(x, y, true);
                    } else if (command == "клик") {
                        std::string button;
                        iss >> button;
                        clickMouse(button, true);
                        Sleep(10);
                        clickMouse(button, false);
                    }
                }
            }).detach();

            return 1;
        }
    }
    return CallNextHookEx(keyboardHook, nCode, wParam, lParam);
}

// Tray icon message handler
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_USER + 1) {
        if (lParam == WM_RBUTTONDOWN) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"Pause");
            AppendMenuW(hMenu, MF_STRING, 2, L"Resume");
            AppendMenuW(hMenu, MF_STRING, 3, L"Terminate");

            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
    } else if (uMsg == WM_COMMAND) {
        if (LOWORD(wParam) == 1) {
            isPaused = true;
        } else if (LOWORD(wParam) == 2) {
            isPaused = false;
        } else if (LOWORD(wParam) == 3) {
            running = false;
            PostQuitMessage(0);
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Function to setup tray icon
void setupTrayIcon() {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TrayWindowClass";
    RegisterClassW(&wc);

    hwnd = CreateWindowW(L"TrayWindowClass", NULL, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 1;
    nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SEEKER));
    wcscpy(nid.szTip, L"Seeker Script");
    Shell_NotifyIcon(NIM_ADD, &nid);
}

// Function to parse and execute script with bindings (recursive for includes)
void executeScriptWithBindings(const std::wstring& filename, std::set<std::wstring>& includedFiles) {
    // Prevent circular includes
    if (includedFiles.find(filename) != includedFiles.end()) {
        return; // Already included, skip
    }
    includedFiles.insert(filename);

    std::wifstream file(filename);
    if (!file.is_open()) {
        std::wstring errorMsg = L"Cannot open script file: " + filename;
        MessageBox(NULL, errorMsg.c_str(), L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Set locale for UTF-8 support
    std::locale loc(std::locale(), new std::codecvt_utf8<wchar_t>);
    file.imbue(loc);

    std::wstring wline;
    int currentKey = -1;
    std::vector<std::string> loopCommands;
    bool inLoop = false;
    int loopIterations = 0;

    while (std::getline(file, wline) && running) {
        // Remove comments
        size_t commentPos = wline.find(L'#');
        if (commentPos != std::wstring::npos) {
            std::wstring commentPart = wline.substr(commentPos + 1);
            // Check for include directive
            size_t includePos = commentPart.find(L"include");
            if (includePos != std::wstring::npos) {
                // Extract filename from #include "filename"
                size_t quote1 = commentPart.find(L'"', includePos);
                if (quote1 != std::wstring::npos) {
                    size_t quote2 = commentPart.find(L'"', quote1 + 1);
                    if (quote2 != std::wstring::npos) {
                        std::wstring includeFilename = commentPart.substr(quote1 + 1, quote2 - quote1 - 1);
                        // Convert relative path to absolute if needed
                        std::wstring fullPath = includeFilename;
                        if (includeFilename.find(L':') == std::wstring::npos && includeFilename.find(L'\\') == std::wstring::npos) {
                            // Relative path - get directory of current file
                            size_t lastSlash = filename.find_last_of(L"\\/");
                            if (lastSlash != std::wstring::npos) {
                                fullPath = filename.substr(0, lastSlash + 1) + includeFilename;
                            }
                        }
                        executeScriptWithBindings(fullPath, includedFiles);
                    }
                }
            }
            wline = wline.substr(0, commentPos);
        }

        // Trim whitespace
        wline.erase(0, wline.find_first_not_of(L" \t"));
        wline.erase(wline.find_last_not_of(L" \t") + 1);

        if (wline.empty()) continue;

        // Convert wstring to string for parsing
        std::string line(wline.begin(), wline.end());

        std::istringstream iss(line);
        std::string command;
        iss >> command;

        if (inLoop) {
            if (command == "конец_залупы") {
                inLoop = false;
                executeLoop(loopCommands, loopIterations);
                loopCommands.clear();
            } else {
                loopCommands.push_back(line);
            }
            continue;
        }

        if (command == "bind") {
            std::string keyStr;
            iss >> keyStr;
            if (keyStr.back() == ':') {
                keyStr.pop_back();
            }
            if (!keyStr.empty()) {
                if (keyStr == "k" || keyStr == "K") {
                    currentKey = VK_K;
                } else {
                    currentKey = VkKeyScanA(keyStr[0]) & 0xFF;
                }
                keyCommands[currentKey] = std::vector<std::string>();
            }
        } else if (command == "::") {
            currentKey = -1;
        } else if (currentKey != -1) {
            keyCommands[currentKey].push_back(line);
        } else if (command == "zalupa") {
            iss >> loopIterations;
            inLoop = true;
            loopCommands.clear();
        } else if (command == "otpravit") {
            std::string text;
            std::getline(iss, text);
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](int ch) { return !std::isspace(ch); }));
            sendKeys(text);
        } else if (command == "спящий") {
            int ms;
            iss >> ms;
            Sleep(ms);
        } else if (command == "мышь") {
            int x, y;
            iss >> x >> y;
            moveMouse(x, y, false);
        } else if (command == "абсолютная_мышь") {
            int x, y;
            iss >> x >> y;
            moveMouse(x, y, true);
        } else if (command == "клик") {
            std::string button;
            iss >> button;
            clickMouse(button, true);
            Sleep(10);
            clickMouse(button, false);
        }
    }
}

// Installer dialog procedure
INT_PTR CALLBACK InstallerDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG: {
            // Set window icon
            SendMessage(hwndDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SEEKER)));
            
            // Set default selections
            CheckDlgButton(hwndDlg, IDC_UNICODE, BST_CHECKED);
            CheckDlgButton(hwndDlg, IDC_STARTMENU, BST_CHECKED);
            CheckDlgButton(hwndDlg, IDC_DESKTOP, BST_CHECKED);
            return TRUE;
        }
        
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDC_INSTALL_BTN: {
                    BOOL createStartMenu = IsDlgButtonChecked(hwndDlg, IDC_STARTMENU);
                    BOOL createDesktop = IsDlgButtonChecked(hwndDlg, IDC_DESKTOP);
                    
                    install(createStartMenu, createDesktop);
                    
                    MessageBox(hwndDlg, 
                        L"Seeker Script has been successfully installed!\n\n"
                        L".sekeer files are now associated with this application.",
                        L"Installation Complete", 
                        MB_OK | MB_ICONINFORMATION);
                    
                    EndDialog(hwndDlg, 0);
                    return TRUE;
                }
                
                case IDC_CANCEL_BTN: {
                    EndDialog(hwndDlg, 0);
                    return TRUE;
                }
            }
            break;
        }
        
        case WM_CLOSE: {
            EndDialog(hwndDlg, 0);
            return TRUE;
        }
    }
    return FALSE;
}

// Show installer dialog
void showInstaller() {
    DialogBox(hInstance, MAKEINTRESOURCE(IDD_INSTALLER), NULL, InstallerDialogProc);
}

// Entry point
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    hInstance = hInst;
    
    // Initialize common controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);
    
    // Parse command line
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    
    if (argc > 1) {
        std::wstring arg = argv[1];
        if (arg == L"--install") {
            showInstaller();
        } else if (arg == L"--uninstall") {
            uninstall();
            MessageBox(NULL, L"Seeker Script has been successfully uninstalled!", L"Uninstallation Complete", MB_OK | MB_ICONINFORMATION);
        } else if (arg.find(L".sekeer") != std::wstring::npos) {
            std::wstring scriptFile = arg;

            setupTrayIcon();
            keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);

            std::set<std::wstring> includedFiles;
            scriptThread = std::thread(executeScriptWithBindings, scriptFile, std::ref(includedFiles));

            MSG msg;
            while (GetMessage(&msg, NULL, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            if (keyboardHook) UnhookWindowsHookEx(keyboardHook);
            Shell_NotifyIcon(NIM_DELETE, &nid);
            running = false;
            if (scriptThread.joinable()) scriptThread.join();
        } else {
            showInstaller();
        }
    } else {
        showInstaller();
    }
    
    LocalFree(argv);
    return 0;
}