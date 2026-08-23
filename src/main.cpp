// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <string>
#include <memory>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#include "AudioSessionMuter.h"
#include "Config.h"
#include "AppUI.h"
#include "Utils.h"
#include "resource.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Dwmapi.lib")

using Microsoft::WRL::ComPtr;

#define WM_TRAYICON (WM_USER + 100)
#define WM_APP_MUTE_NOTIFY (WM_APP + 1)
#define WM_APP_DEVICE_CHANGED (WM_APP + 2)

#define ID_TRAY_SHOW 1001
#define ID_TRAY_AUTOSTART 1002
#define ID_TRAY_UNMUTE_ON_EXIT 1003
#define ID_TRAY_EXIT 1004

#define IDT_DEVICE_REFRESH 2001

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

// Notification payload sent from background MTA thread to STA UI thread
struct MuteNotificationPayload {
    std::wstring procName;
    DWORD pid = 0;
};

// Global DirectX 11 ComPtr objects
static ComPtr<ID3D11Device>            g_pd3dDevice;
static ComPtr<ID3D11DeviceContext>     g_pd3dDeviceContext;
static ComPtr<IDXGISwapChain>          g_pSwapChain;
static ComPtr<ID3D11RenderTargetView>  g_mainRenderTargetView;

static HWND                            g_hWnd = nullptr;
static NOTIFYICONDATAW                 g_nid = { 0 };
static bool                            g_isWindowVisible = true;

// AppUI reference for thread messages
static AppUI*                          g_pGlobalAppUI = nullptr;
static AppConfig*                      g_pGlobalConfig = nullptr;

// Function prototypes
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void InitTrayIcon(HWND hWnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    wcscpy_s(g_nid.szTip, L"SonarCord - Audio Session Muter");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowTrayNotification(const std::wstring& title, const std::wstring& message) {
    if (!g_pGlobalConfig || !g_pGlobalConfig->showNotifications) return;
    
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags |= NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, message.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowTrayMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, g_isWindowVisible ? L"Hide Dashboard" : L"Show Dashboard");
    
    bool autoStart = AppConfig::IsAutoStartEnabled();
    AppendMenuW(hMenu, MF_STRING | (autoStart ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_AUTOSTART, L"Start with Windows");

    bool unmuteOnExit = g_pGlobalConfig ? g_pGlobalConfig->unmuteOnExit : true;
    AppendMenuW(hMenu, MF_STRING | (unmuteOnExit ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_UNMUTE_ON_EXIT, L"Unmute on Exit");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    SetForegroundWindow(hWnd);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == ID_TRAY_SHOW) {
        if (g_isWindowVisible) {
            ShowWindow(hWnd, SW_HIDE);
            g_isWindowVisible = false;
        } else {
            if (g_pGlobalAppUI) {
                g_pGlobalAppUI->SyncUIFromConfig();
            }
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            g_isWindowVisible = true;
        }
    } else if (cmd == ID_TRAY_AUTOSTART) {
        bool newStatus = !AppConfig::IsAutoStartEnabled();
        AppConfig::SetAutoStart(newStatus);
        if (g_pGlobalConfig) {
            g_pGlobalConfig->startWithWindows = newStatus;
            g_pGlobalConfig->Save();
        }
        if (g_pGlobalAppUI) {
            g_pGlobalAppUI->SyncUIFromConfig();
        }
    } else if (cmd == ID_TRAY_UNMUTE_ON_EXIT) {
        if (g_pGlobalConfig) {
            g_pGlobalConfig->unmuteOnExit = !g_pGlobalConfig->unmuteOnExit;
            g_pGlobalConfig->Save();
        }
        if (g_pGlobalAppUI) {
            g_pGlobalAppUI->SyncUIFromConfig();
        }
    } else if (cmd == ID_TRAY_EXIT) {
        PostQuitMessage(0);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nShowCmd) {
    // Enable Per-Monitor V2 DPI Awareness for crisp UI on high-DPI displays
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 0. Single Instance Check via Named Mutex
    HANDLE hSingleInstanceMutex = CreateMutexW(NULL, TRUE, L"Local\\SonarCord_SingleInstance_Mutex");
    if (hSingleInstanceMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingWnd = FindWindowW(L"SonarCordClass", NULL);
        if (existingWnd) {
            ShowWindow(existingWnd, SW_SHOW);
            SetForegroundWindow(existingWnd);
        }
        CloseHandle(hSingleInstanceMutex);
        return 0;
    }

    // 1. Load Configuration
    AppConfig config;
    config.Load();
    g_pGlobalConfig = &config;

    // 2. Initialize Audio Session Muter
    AudioSessionMuter muter;
    if (!muter.Initialize()) {
        MessageBoxW(NULL, L"Failed to initialize Audio Session Muter!", L"SonarCord Error", MB_ICONERROR);
        if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
        return 1;
    }
    muter.SetTargetProcess(config.targetProcessName);

    // Notification Callback: Post to UI thread to guarantee COM/Tray thread-safety
    muter.SetNotificationCallback([](const std::wstring& procName, DWORD pid) {
        if (g_hWnd) {
            auto* payload = new MuteNotificationPayload{ procName, pid };
            if (PostMessageW(g_hWnd, WM_APP_MUTE_NOTIFY, reinterpret_cast<WPARAM>(payload), 0) == 0) {
                delete payload;
            }
        }
    });

    // Device Hotplug Callback: Post to UI thread
    muter.SetDeviceChangedCallback([]() {
        if (g_hWnd) {
            PostMessageW(g_hWnd, WM_APP_DEVICE_CHANGED, 0, 0);
        }
    });

    // 3. Create Win32 Window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, NULL, NULL, NULL, NULL, L"SonarCordClass", NULL };
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Failed to register window class!", L"SonarCord Error", MB_ICONERROR);
        muter.Shutdown(config.unmuteOnExit);
        if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
        return 1;
    }

    int winWidth = 480;
    int winHeight = 650;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - winWidth) / 2;
    int posY = (screenH - winHeight) / 2;

    g_hWnd = CreateWindowW(wc.lpszClassName, L"SonarCord", 
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
                           posX, posY, winWidth, winHeight, 
                           NULL, NULL, wc.hInstance, NULL);

    if (!g_hWnd) {
        MessageBoxW(NULL, L"Failed to create window!", L"SonarCord Error", MB_ICONERROR);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        muter.Shutdown(config.unmuteOnExit);
        if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
        return 1;
    }

    // Dark Titlebar (DWM Dark Mode)
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    COLORREF captionColor = RGB(20, 22, 28);
    DwmSetWindowAttribute(g_hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    if (!CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        DestroyWindow(g_hWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        muter.Shutdown(config.unmuteOnExit);
        if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
        return 1;
    }

    // 4. Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Disable dummy imgui.ini file generation

    // Load Segoe UI Font with upward baseline adjustment to center text vertically
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 3;
    fontConfig.PixelSnapH = false;
    fontConfig.GlyphOffset.y = -1.0f; // Optical vertical centering for Segoe UI
    
    char fontPath[MAX_PATH];
    if (!ExpandEnvironmentStringsA("%WINDIR%\\Fonts\\segoeui.ttf", fontPath, MAX_PATH)) {
        strcpy_s(fontPath, "C:\\Windows\\Fonts\\segoeui.ttf");
    }
    
    FILE* f = nullptr;
    if (fopen_s(&f, fontPath, "r") == 0 && f != nullptr) {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(fontPath, 17.5f, &fontConfig, io.Fonts->GetGlyphRangesDefault());
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice.Get(), g_pd3dDeviceContext.Get());

    // 5. Initialize UI Controller
    AppUI appUI(&muter, &config);
    appUI.Initialize();
    g_pGlobalAppUI = &appUI;

    // Initialize System Tray Icon
    InitTrayIcon(g_hWnd);

    // Check Start Minimized
    std::wstring cmdLine(lpCmdLine ? lpCmdLine : L"");
    bool startMinimized = (cmdLine.find(L"--minimized") != std::wstring::npos) || config.startMinimized;

    if (startMinimized) {
        ShowWindow(g_hWnd, SW_HIDE);
        g_isWindowVisible = false;
    } else {
        ShowWindow(g_hWnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hWnd);
        g_isWindowVisible = true;
    }

    // 6. Intelligent Power-Saving Message Loop
    bool done = false;
    while (!done) {
        MSG msg;
        if (g_isWindowVisible) {
            while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;

            // If window was hidden during message processing
            if (!g_isWindowVisible) {
                continue;
            }

            if (appUI.ShouldMinimizeToTray()) {
                appUI.ResetMinimizeRequest();
                ShowWindow(g_hWnd, SW_HIDE);
                g_isWindowVisible = false;
                continue;
            }

            // Render ImGui Frame
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            appUI.Render();

            ImGui::Render();
            const float clear_color_with_alpha[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
            ID3D11RenderTargetView* rtv = g_mainRenderTargetView.Get();
            g_pd3dDeviceContext->OMSetRenderTargets(1, &rtv, NULL);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView.Get(), clear_color_with_alpha);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            g_pSwapChain->Present(1, 0); // V-Sync
        } else {
            // Suspended / Waiting in Tray (0.00% CPU usage & minimal RAM)
            BOOL bRet = GetMessage(&msg, NULL, 0, 0);
            if (bRet == 0) {
                done = true; // WM_QUIT
            } else if (bRet == -1) {
                done = true; // Error occurred
            } else {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    // Cleanup
    g_pGlobalAppUI = nullptr;
    g_pGlobalConfig = nullptr;

    RemoveTrayIcon();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    muter.Shutdown(config.unmuteOnExit);

    if (hSingleInstanceMutex) {
        CloseHandle(hSingleInstanceMutex);
    }
    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, 
                                                featureLevelArray, 2, D3D11_SDK_VERSION, &sd, 
                                                &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, createDeviceFlags, 
                                            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, 
                                            &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    g_pSwapChain.Reset();
    g_pd3dDeviceContext.Reset();
    g_pd3dDevice.Reset();
}

void CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> pBackBuffer;
    if (g_pSwapChain) {
        HRESULT hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (SUCCEEDED(hr) && pBackBuffer) {
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), NULL, g_mainRenderTargetView.ReleaseAndGetAddressOf());
        }
    }
}

void CleanupRenderTarget() {
    g_mainRenderTargetView.Reset();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_APP_MUTE_NOTIFY: {
        auto* payload = reinterpret_cast<MuteNotificationPayload*>(wParam);
        if (payload) {
            std::wstring msgText = payload->procName + L" (PID: " + std::to_wstring(payload->pid) + L") session was automatically muted.";
            ShowTrayNotification(L"SonarCord", msgText);
            delete payload;
        }
        return 0;
    }

    case WM_APP_DEVICE_CHANGED: {
        // Debounce rapid-fire hotplug events (coalesce multiple events into 1 refresh after 250ms)
        SetTimer(hWnd, IDT_DEVICE_REFRESH, 250, NULL);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == IDT_DEVICE_REFRESH) {
            KillTimer(hWnd, IDT_DEVICE_REFRESH);
            if (g_pGlobalAppUI) {
                g_pGlobalAppUI->RefreshDevices();
            }
        }
        return 0;
    }

    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hWnd, SW_HIDE);
            g_isWindowVisible = false;
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_MINIMIZE) {
            ShowWindow(hWnd, SW_HIDE);
            g_isWindowVisible = false;
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        g_isWindowVisible = false;
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            if (g_isWindowVisible) {
                ShowWindow(hWnd, SW_HIDE);
                g_isWindowVisible = false;
            } else {
                if (g_pGlobalAppUI) {
                    g_pGlobalAppUI->SyncUIFromConfig();
                }
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
                g_isWindowVisible = true;
            }
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hWnd);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
