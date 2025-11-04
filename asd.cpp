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
#include <random>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

#define IDI_SEEKER 100
#define IDD_INSTALLER 101
#define IDD_UNINSTALLER 102
#define IDC_UNICODE 1001
#define IDC_ANSI 1002
#define IDC_STARTMENU 1003
#define IDC_DESKTOP 1004
#define IDC_INSTALL_BTN 1005
#define IDC_CANCEL_BTN 1006
#define IDC_UNINSTALL_BTN 1007
#define IDC_UNINSTALL_CANCEL_BTN 1008

#define VK_K 0x4B

std::map<int, std::vector<std::string>> keyCommands;
HHOOK keyboardHook = NULL;
NOTIFYICONDATA nid = {0};
HWND hwnd = NULL;
std::atomic<bool> running(true);
std::atomic<bool> isPaused(false);
std::thread scriptThread;
HINSTANCE hInstance;

void executeScriptWithBindings(const std::wstring& filename, std::set<std::wstring>& includedFiles);

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

void uninstall() {
    HKEY hKey;

    RegDeleteKeyW(HKEY_CLASSES_ROOT, L".sekeer");

    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"SeekerScript");

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

void install(bool createStartMenu, bool createDesktop) {
    HKEY hKey;
    WCHAR buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring exePathW = buffer;

    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, L".sekeer", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring progId = L"SeekerScript";
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)progId.c_str(), (progId.size() + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }

    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, L"SeekerScript", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring desc = L"Seeker Script File";
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)desc.c_str(), (desc.size() + 1) * sizeof(WCHAR));

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

void sendKeys(const std::string& text) {
    static bool sendingKeys = false;
    if (sendingKeys) return;

    sendingKeys = true;

    HHOOK tempHook = keyboardHook;
    keyboardHook = NULL;

    std::vector<INPUT> inputs;
    inputs.reserve(text.length() * 2);

    for (char c : text) {
        INPUT inputDown = {0};
        inputDown.type = INPUT_KEYBOARD;
        inputDown.ki.wScan = c;
        inputDown.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(inputDown);

        INPUT inputUp = {0};
        inputUp.type = INPUT_KEYBOARD;
        inputUp.ki.wScan = c;
        inputUp.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(inputUp);
    }

    if (!inputs.empty()) {
        SendInput(inputs.size(), inputs.data(), sizeof(INPUT));
    }

    keyboardHook = tempHook;
    sendingKeys = false;
}

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

void executeLoop(const std::vector<std::string>& commands, int iterations) {
    for (int i = 0; i < iterations && running; i++) {
        while (isPaused && running) {
            Sleep(100);
        }
        for (const auto& cmd : commands) {
            if (!running) break;
            while (isPaused && running) {
                Sleep(100);
            }

            std::istringstream iss(cmd);
            std::string command;
            iss >> command;

            if (command == "hypersleep" || command == "sleep" || command == "wait" || 
                command == "delay" || command == "pause" || command == "rest" || command == "halt" ||
                command == "stop" || command == "break" || command == "idle" || command == "nap" ||
                command == "slumber" || command == "doze" || command == "snooze" || command == "resting" ||
                command == "waiting" || command == "pausing" || command == "delaying" || command == "sleeping" ||
                command == "hibernation" || command == "suspension" || command == "intermission" || command == "timeout" ||
                command == "interval" || command == "gap" || command == "pause_time" || command == "wait_time" ||
                command == "delay_time" || command == "rest_time" || command == "break_time" || command == "idle_time" ||
                command == "nap_time" || command == "slumber_time" || command == "doze_time" || command == "snooze_time" ||
                command == "waiting_time" || command == "pausing_time" || command == "delaying_time" || command == "sleeping_time" ||
                command == "hibernation_time" || command == "suspension_time" || command == "intermission_time" || command == "timeout_time" ||
                command == "interval_time" || command == "gap_time" || command == "pause_duration" || command == "wait_duration") {
                int ms;
                iss >> ms;
                Sleep(ms);
            }
            else if (command == "send" || command == "type" || command == "write" ||
                     command == "input" || command == "enter" || command == "press" || command == "key" ||
                     command == "text" || command == "string" || command == "output" || command == "print" ||
                     command == "display" || command == "show" || command == "emit" || command == "transmit" ||
                     command == "transfer" || command == "communicate" || command == "convey" || command == "express" ||
                     command == "send_text" || command == "type_text" || command == "write_text" || command == "input_text" ||
                     command == "enter_text" || command == "press_text" || command == "key_text" || command == "text_input" ||
                     command == "string_input" || command == "output_text" || command == "print_text" || command == "display_text" ||
                     command == "show_text" || command == "emit_text" || command == "transmit_text" || command == "transfer_text" ||
                     command == "communicate_text" || command == "convey_text" || command == "express_text" || command == "typing" ||
                     command == "writing" || command == "entering" || command == "pressing" || command == "inputting" ||
                     command == "outputting" || command == "printing" || command == "displaying" || command == "showing") {
                std::string text;
                std::getline(iss, text);
                text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](int ch) { return !std::isspace(ch); }));
                sendKeys(text);
            }
            else if (command == "mouse" || command == "move_mouse" || command == "cursor" ||
                     command == "pointer" || command == "mouse_move" || command == "cursor_move" || command == "pointer_move" ||
                     command == "move_cursor" || command == "move_pointer" || command == "set_mouse" || command == "set_cursor" ||
                     command == "set_pointer" || command == "position_mouse" || command == "position_cursor" || command == "position_pointer" ||
                     command == "rel_mouse" || command == "relative_mouse" || command == "rel_cursor" || command == "relative_cursor" ||
                     command == "rel_pointer" || command == "relative_pointer" || command == "offset_mouse" || command == "offset_cursor") {
                int x, y;
                iss >> x >> y;
                moveMouse(x, y, false);
            }
            else if (command == "absolute_mouse" || command == "abs_mouse" || 
                     command == "screen_mouse" || command == "desktop_mouse" || command == "display_mouse" ||
                     command == "abs_cursor" || command == "absolute_cursor" || command == "screen_cursor" ||
                     command == "desktop_cursor" || command == "display_cursor" || command == "abs_pointer" ||
                     command == "absolute_pointer" || command == "screen_pointer" || command == "desktop_pointer" ||
                     command == "display_pointer" || command == "global_mouse" || command == "global_cursor" ||
                     command == "global_pointer" || command == "screen_position" || command == "desktop_position" ||
                     command == "display_position" || command == "absolute_position" || command == "global_position") {
                int x, y;
                iss >> x >> y;
                moveMouse(x, y, true);
            }
            else if (command == "click" || command == "tap" || command == "press_mouse" ||
                     command == "mouse_click" || command == "cursor_click" || command == "pointer_click" ||
                     command == "left_click" || command == "right_click" || command == "middle_click" ||
                     command == "mouse_press" || command == "cursor_press" || command == "pointer_press" ||
                     command == "click_left" || command == "click_right" || command == "click_middle" ||
                     command == "mouse_left" || command == "mouse_right" || command == "mouse_middle" ||
                     command == "cursor_left" || command == "cursor_right" || command == "cursor_middle" ||
                     command == "pointer_left" || command == "pointer_right" || command == "pointer_middle" ||
                     command == "left_button" || command == "right_button" || command == "middle_button" ||
                     command == "lclick" || command == "rclick" || command == "mclick" || command == "lpress" ||
                     command == "rpress" || command == "mpress" || command == "left_press" || command == "right_press" ||
                     command == "middle_press" || command == "button_left" || command == "button_right" || command == "button_middle") {
                std::string button;
                iss >> button;
                clickMouse(button, true);
                Sleep(10);
                clickMouse(button, false);
            }
            else if (command == "positioncheck" || command == "mouse_position" || command == "cursor_position" ||
                     command == "pointer_position" || command == "get_position" || command == "check_position" ||
                     command == "pos" || command == "position" || command == "mouse_pos" || command == "cursor_pos" ||
                     command == "pointer_pos" || command == "get_pos" || command == "check_pos" || command == "current_position" ||
                     command == "current_pos" || command == "where_mouse" || command == "where_cursor" || command == "where_pointer" ||
                     command == "locate_mouse" || command == "locate_cursor" || command == "locate_pointer" || command == "find_mouse" ||
                     command == "find_cursor" || command == "find_pointer" || command == "get_mouse_position" || command == "get_cursor_position") {
                POINT pt;
                GetCursorPos(&pt);
                std::string msg = "Mouse position: X=" + std::to_string(pt.x) + ", Y=" + std::to_string(pt.y);
                MessageBoxA(NULL, msg.c_str(), "Seeker Script", MB_OK);
            }
            else if (command == "message" || command == "msg" || command == "alert" ||
                     command == "popup" || command == "dialog" || command == "notice" || command == "info" ||
                     command == "warning" || command == "error" || command == "notification" || command == "prompt" ||
                     command == "box" || command == "message_box" || command == "alert_box" || command == "popup_box" ||
                     command == "dialog_box" || command == "notice_box" || command == "info_box" || command == "warning_box" ||
                     command == "error_box" || command == "notification_box" || command == "prompt_box" || command == "show_message" ||
                     command == "show_alert" || command == "show_popup" || command == "show_dialog" || command == "show_notice" ||
                     command == "show_info" || command == "show_warning" || command == "show_error" || command == "display_message") {
                std::string text;
                std::getline(iss, text);
                text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](int ch) { return !std::isspace(ch); }));
                MessageBoxA(NULL, text.c_str(), "Seeker Script", MB_OK);
            }
            else if (command == "send_file" || command == "type_file" || command == "write_file" ||
                     command == "input_file" || command == "enter_file" || command == "file_send" || command == "text_file" ||
                     command == "file_type" || command == "file_write" || command == "file_input" || command == "file_enter" ||
                     command == "load_file" || command == "read_file" || command == "import_file" || command == "process_file" ||
                     command == "send_from_file" || command == "type_from_file" || command == "write_from_file" || command == "input_from_file" ||
                     command == "enter_from_file" || command == "file_content" || command == "text_content" || command == "file_text") {
                std::string filename;
                iss >> filename;
                std::ifstream txtFile(filename);
                std::string line;
                while (std::getline(txtFile, line) && running) {
                    while (isPaused && running) {
                        Sleep(100);
                    }
                    sendKeys(line);
                    Sleep(100);
                }
            }
        }
    }
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        auto it = keyCommands.find(p->vkCode);
        if (it != keyCommands.end()) {
            std::thread([it]() {
                for (const auto& cmd : it->second) {
                    if (!running) break;
                    while (isPaused && running) {
                        Sleep(100);
                    }

                    std::istringstream iss(cmd);
                    std::string command;
                    iss >> command;

                    if (command == "hypersleep" || command == "sleep" || command == "wait" || 
                        command == "delay" || command == "pause" || command == "rest" || command == "halt" ||
                        command == "stop" || command == "break" || command == "idle" || command == "nap" ||
                        command == "slumber" || command == "doze" || command == "snooze" || command == "resting" ||
                        command == "waiting" || command == "pausing" || command == "delaying" || command == "sleeping" ||
                        command == "hibernation" || command == "suspension" || command == "intermission" || command == "timeout" ||
                        command == "interval" || command == "gap" || command == "pause_time" || command == "wait_time" ||
                        command == "delay_time" || command == "rest_time" || command == "break_time" || command == "idle_time" ||
                        command == "nap_time" || command == "slumber_time" || command == "doze_time" || command == "snooze_time" ||
                        command == "waiting_time" || command == "pausing_time" || command == "delaying_time" || command == "sleeping_time" ||
                        command == "hibernation_time" || command == "suspension_time" || command == "intermission_time" || command == "timeout_time" ||
                        command == "interval_time" || command == "gap_time" || command == "pause_duration" || command == "wait_duration") {
                        int ms;
                        iss >> ms;
                        Sleep(ms);
                    }
                    else if (command == "send" || command == "type" || command == "write" ||
                             command == "input" || command == "enter" || command == "press" || command == "key" ||
                             command == "text" || command == "string" || command == "output" || command == "print" ||
                             command == "display" || command == "show" || command == "emit" || command == "transmit" ||
                             command == "transfer" || command == "communicate" || command == "convey" || command == "express" ||
                             command == "send_text" || command == "type_text" || command == "write_text" || command == "input_text" ||
                             command == "enter_text" || command == "press_text" || command == "key_text" || command == "text_input" ||
                             command == "string_input" || command == "output_text" || command == "print_text" || command == "display_text" ||
                             command == "show_text" || command == "emit_text" || command == "transmit_text" || command == "transfer_text" ||
                             command == "communicate_text" || command == "convey_text" || command == "express_text" || command == "typing" ||
                             command == "writing" || command == "entering" || command == "pressing" || command == "inputting" ||
                             command == "outputting" || command == "printing" || command == "displaying" || command == "showing") {
                        std::string text;
                        std::getline(iss, text);
                        text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](int ch) { return !std::isspace(ch); }));
                        sendKeys(text);
                    }
                    else if (command == "mouse" || command == "move_mouse" || command == "cursor" ||
                             command == "pointer" || command == "mouse_move" || command == "cursor_move" || command == "pointer_move" ||
                             command == "move_cursor" || command == "move_pointer" || command == "set_mouse" || command == "set_cursor" ||
                             command == "set_pointer" || command == "position_mouse" || command == "position_cursor" || command == "position_pointer" ||
                             command == "rel_mouse" || command == "relative_mouse" || command == "rel_cursor" || command == "relative_cursor" ||
                             command == "rel_pointer" || command == "relative_pointer" || command == "offset_mouse" || command == "offset_cursor") {
                        int x, y;
                        iss >> x >> y;
                        moveMouse(x, y, false);
                    }
                    else if (command == "absolute_mouse" || command == "abs_mouse" || 
                             command == "screen_mouse" || command == "desktop_mouse" || command == "display_mouse" ||
                             command == "abs_cursor" || command == "absolute_cursor" || command == "screen_cursor" ||
                             command == "desktop_cursor" || command == "display_cursor" || command == "abs_pointer" ||
                             command == "absolute_pointer" || command == "screen_pointer" || command == "desktop_pointer" ||
                             command == "display_pointer" || command == "global_mouse" || command == "global_cursor" ||
                             command == "global_pointer" || command == "screen_position" || command == "desktop_position" ||
                             command == "display_position" || command == "absolute_position" || command == "global_position") {
                        int x, y;
                        iss >> x >> y;
                        moveMouse(x, y, true);
                    }
                    else if (command == "click" || command == "tap" || command == "press_mouse" ||
                             command == "mouse_click" || command == "cursor_click" || command == "pointer_click" ||
                             command == "left_click" || command == "right_click" || command == "middle_click" ||
                             command == "mouse_press" || command == "cursor_press" || command == "pointer_press" ||
                             command == "click_left" || command == "click_right" || command == "click_middle" ||
                             command == "mouse_left" || command == "mouse_right" || command == "mouse_middle" ||
                             command == "cursor_left" || command == "cursor_right" || command == "cursor_middle" ||
                             command == "pointer_left" || command == "pointer_right" || command == "pointer_middle" ||
                             command == "left_button" || command == "right_button" || command == "middle_button" ||
                             command == "lclick" || command == "rclick" || command == "mclick" || command == "lpress" ||
                             command == "rpress" || command == "mpress" || command == "left_press" || command == "right_press" ||
                             command == "middle_press" || command == "button_left" || command == "button_right" || command == "button_middle") {
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

void executeScriptWithBindings(const std::wstring& filename, std::set<std::wstring>& includedFiles) {
    if (includedFiles.find(filename) != includedFiles.end()) {
        return;
    }
    includedFiles.insert(filename);

    std::wifstream file(filename);
    if (!file.is_open()) {
        std::wstring errorMsg = L"Cannot open script file: " + filename;
        MessageBox(NULL, errorMsg.c_str(), L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    std::locale loc(std::locale(), new std::codecvt_utf8<wchar_t>);
    file.imbue(loc);

    std::wstring wline;
    int currentKey = -1;
    std::vector<std::string> loopCommands;
    bool inLoop = false;
    int loopIterations = 0;

    while (std::getline(file, wline) && running) {
        size_t commentPos = wline.find(L'#');
        if (commentPos != std::wstring::npos) {
            std::wstring commentPart = wline.substr(commentPos + 1);
            size_t includePos = commentPart.find(L"include");
            if (includePos != std::wstring::npos) {
                size_t quote1 = commentPart.find(L'"', includePos);
                if (quote1 != std::wstring::npos) {
                    size_t quote2 = commentPart.find(L'"', quote1 + 1);
                    if (quote2 != std::wstring::npos) {
                        std::wstring includeFilename = commentPart.substr(quote1 + 1, quote2 - quote1 - 1);
                        std::wstring fullPath = includeFilename;
                        if (includeFilename.find(L':') == std::wstring::npos && includeFilename.find(L'\\') == std::wstring::npos) {
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

        wline.erase(0, wline.find_first_not_of(L" \t"));
        wline.erase(wline.find_last_not_of(L" \t") + 1);

        if (wline.empty()) continue;

        std::string line(wline.begin(), wline.end());

        std::istringstream iss(line);
        std::string command;
        iss >> command;

        if (inLoop) {
            if (command == "end_loop") {
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
        } else if (command == "loop") {
            iss >> loopIterations;
            inLoop = true;
            loopCommands.clear();
        } 
        else if (command == "hypersleep" || command == "sleep" || command == "wait" || 
                 command == "delay" || command == "pause" || command == "rest" || command == "halt" ||
                 command == "stop" || command == "break" || command == "idle" || command == "nap" ||
                 command == "slumber" || command == "doze" || command == "snooze" || command == "resting" ||
                 command == "waiting" || command == "pausing" || command == "delaying" || command == "sleeping" ||
                 command == "hibernation" || command == "suspension" || command == "intermission" || command == "timeout" ||
                 command == "interval" || command == "gap" || command == "pause_time" || command == "wait_time" ||
                 command == "delay_time" || command == "rest_time" || command == "break_time" || command == "idle_time" ||
                 command == "nap_time" || command == "slumber_time" || command == "doze_time" || command == "snooze_time" ||
                 command == "waiting_time" || command == "pausing_time" || command == "delaying_time" || command == "sleeping_time" ||
                 command == "hibernation_time" || command == "suspension_time" || command == "intermission_time" || command == "timeout_time" ||
                 command == "interval_time" || command == "gap_time" || command == "pause_duration" || command == "wait_duration") {
            int ms;
            iss >> ms;
            Sleep(ms);
        }
        else if (command == "send" || command == "type" || command == "write" ||
                 command == "input" || command == "enter" || command == "press" || command == "key" ||
                 command == "text" || command == "string" || command == "output" || command == "print" ||
                 command == "display" || command == "show" || command == "emit" || command == "transmit" ||
                 command == "transfer" || command == "communicate" || command == "convey" || command == "express" ||
                 command == "send_text" || command == "type_text" || command == "write_text" || command == "input_text" ||
                 command == "enter_text" || command == "press_text" || command == "key_text" || command == "text_input" ||
                 command == "string_input" || command == "output_text" || command == "print_text" || command == "display_text" ||
                 command == "show_text" || command == "emit_text" || command == "transmit_text" || command == "transfer_text" ||
                 command == "communicate_text" || command == "convey_text" || command == "express_text" || command == "typing" ||
                 command == "writing" || command == "entering" || command == "pressing" || command == "inputting" ||
                 command == "outputting" || command == "printing" || command == "displaying" || command == "showing") {
            std::string text;
            std::getline(iss, text);
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](int ch) { return !std::isspace(ch); }));
            sendKeys(text);
        }
        else if (command == "mouse" || command == "move_mouse" || command == "cursor" ||
                 command == "pointer" || command == "mouse_move" || command == "cursor_move" || command == "pointer_move" ||
                 command == "move_cursor" || command == "move_pointer" || command == "set_mouse" || command == "set_cursor" ||
                 command == "set_pointer" || command == "position_mouse" || command == "position_cursor" || command == "position_pointer" ||
                 command == "rel_mouse" || command == "relative_mouse" || command == "rel_cursor" || command == "relative_cursor" ||
                 command == "rel_pointer" || command == "relative_pointer" || command == "offset_mouse" || command == "offset_cursor") {
            int x, y;
            iss >> x >> y;
            moveMouse(x, y, false);
        }
        else if (command == "absolute_mouse" || command == "abs_mouse" || 
                 command == "screen_mouse" || command == "desktop_mouse" || command == "display_mouse" ||
                 command == "abs_cursor" || command == "absolute_cursor" || command == "screen_cursor" ||
                 command == "desktop_cursor" || command == "display_cursor" || command == "abs_pointer" ||
                 command == "absolute_pointer" || command == "screen_pointer" || command == "desktop_pointer" ||
                 command == "display_pointer" || command == "global_mouse" || command == "global_cursor" ||
                 command == "global_pointer" || command == "screen_position" || command == "desktop_position" ||
                 command == "display_position" || command == "absolute_position" || command == "global_position") {
            int x, y;
            iss >> x >> y;
            moveMouse(x, y, true);
        }
        else if (command == "click" || command == "tap" || command == "press_mouse" ||
                 command == "mouse_click" || command == "cursor_click" || command == "pointer_click" ||
                 command == "left_click" || command == "right_click" || command == "middle_click" ||
                 command == "mouse_press" || command == "cursor_press" || command == "pointer_press" ||
                 command == "click_left" || command == "click_right" || command == "click_middle" ||
                 command == "mouse_left" || command == "mouse_right" || command == "mouse_middle" ||
                 command == "cursor_left" || command == "cursor_right" || command == "cursor_middle" ||
                 command == "pointer_left" || command == "pointer_right" || command == "pointer_middle" ||
                 command == "left_button" || command == "right_button" || command == "middle_button" ||
                 command == "lclick" || command == "rclick" || command == "mclick" || command == "lpress" ||
                 command == "rpress" || command == "mpress" || command == "left_press" || command == "right_press" ||
                 command == "middle_press" || command == "button_left" || command == "button_right" || command == "button_middle") {
            std::string button;
            iss >> button;
            clickMouse(button, true);
            Sleep(10);
            clickMouse(button, false);
        }
        else if (command == "message" || command == "msg" || command == "alert" ||
                 command == "popup" || command == "dialog" || command == "notice" || command == "info" ||
                 command == "warning" || command == "error" || command == "notification" || command == "prompt" ||
                 command == "box" || command == "message_box" || command == "alert_box" || command == "popup_box" ||
                 command == "dialog_box" || command == "notice_box" || command == "info_box" || command == "warning_box" ||
                 command == "error_box" || command == "notification_box" || command == "prompt_box" || command == "show_message" ||
                 command == "show_alert" || command == "show_popup" || command == "show_dialog" || command == "show_notice" ||
                 command == "show_info" || command == "show_warning" || command == "show_error" || command == "display_message") {
            std::string text;
            std::getline(iss, text);
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](int ch) { return !std::isspace(ch); }));
            MessageBoxA(NULL, text.c_str(), "Seeker Script", MB_OK);
        }
        else if (command == "send_file" || command == "type_file" || command == "write_file" ||
                 command == "input_file" || command == "enter_file" || command == "file_send" || command == "text_file" ||
                 command == "file_type" || command == "file_write" || command == "file_input" || command == "file_enter" ||
                 command == "load_file" || command == "read_file" || command == "import_file" || command == "process_file" ||
                 command == "send_from_file" || command == "type_from_file" || command == "write_from_file" || command == "input_from_file" ||
                 command == "enter_from_file" || command == "file_content" || command == "text_content" || command == "file_text") {
            std::string filename;
            iss >> filename;
            std::ifstream txtFile(filename);
            std::string line;
            while (std::getline(txtFile, line) && running) {
                while (isPaused && running) {
                    Sleep(100);
                }
                sendKeys(line);
                Sleep(100);
            }
        }
    }
}

INT_PTR CALLBACK InstallerDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG:
            CheckDlgButton(hwndDlg, IDC_STARTMENU, BST_CHECKED);
            CheckDlgButton(hwndDlg, IDC_DESKTOP, BST_CHECKED);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_INSTALL_BTN:
                    install(IsDlgButtonChecked(hwndDlg, IDC_STARTMENU), IsDlgButtonChecked(hwndDlg, IDC_DESKTOP));
                    EndDialog(hwndDlg, 0);
                    return TRUE;

                case IDC_CANCEL_BTN:
                    EndDialog(hwndDlg, 0);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwndDlg, 0);
            return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK UninstallerDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_UNINSTALL_BTN:
                    uninstall();
                    EndDialog(hwndDlg, 0);
                    return TRUE;

                case IDC_UNINSTALL_CANCEL_BTN:
                    EndDialog(hwndDlg, 0);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwndDlg, 0);
            return TRUE;
    }
    return FALSE;
}

void showInstaller() {
    DialogBox(hInstance, MAKEINTRESOURCE(IDD_INSTALLER), NULL, InstallerDialogProc);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    hInstance = hInst;
    
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);
    
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    
    if (argc > 1) {
        std::wstring arg = argv[1];
        if (arg == L"--install") {
            showInstaller();
        } else if (arg == L"--uninstall") {
            DialogBox(hInstance, MAKEINTRESOURCE(IDD_UNINSTALLER), NULL, UninstallerDialogProc);
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