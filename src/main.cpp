// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <memory>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#include "AudioSessionMuter.h"
#include "Config.h"
#include "AppUI.h"
#include "resource.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Dwmapi.lib")

#define WM_TRAYICON (WM_USER + 100)
#define ID_TRAY_SHOW 1001
#define ID_TRAY_AUTOSTART 1002
#define ID_TRAY_EXIT 1003

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

// Global DirectX 11 objects
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static HWND                     g_hWnd = nullptr;
static NOTIFYICONDATAW          g_nid = { 0 };
static bool                     g_isWindowVisible = true;

// AppConfig reference (for tray)
static AppConfig*               g_pGlobalConfig = nullptr;

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
    wcscpy_s(nid.szInfoTitle, title.c_str());
    wcscpy_s(nid.szInfo, message.c_str());
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
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    SetForegroundWindow(hWnd);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == ID_TRAY_SHOW) {
        if (g_isWindowVisible) {
            ShowWindow(hWnd, SW_HIDE);
            g_isWindowVisible = false;
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        } else {
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
    } else if (cmd == ID_TRAY_EXIT) {
        PostQuitMessage(0);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nShowCmd) {
    // 1. Load Configuration
    AppConfig config;
    config.Load();
    g_pGlobalConfig = &config;

    // 2. Initialize Audio Session Muter
    AudioSessionMuter muter;
    if (!muter.Initialize()) {
        MessageBoxW(NULL, L"Failed to initialize Audio Session Muter!", L"SonarCord Error", MB_ICONERROR);
        return 1;
    }

    muter.SetEnabled(config.isEnabled);
    muter.SetTargetProcess(config.targetProcessName);

    // Notification Callback
    muter.SetNotificationCallback([](const std::wstring& procName, DWORD pid) {
        std::wstring msg = procName + L" (PID: " + std::to_wstring(pid) + L") session was automatically muted.";
        ShowTrayNotification(L"SonarCord", msg);
    });

    // 3. Create Win32 Window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, NULL, NULL, NULL, NULL, L"SonarCordClass", NULL };
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

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

    // Dark Titlebar (DWM Dark Mode)
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    COLORREF captionColor = RGB(20, 22, 28);
    DwmSetWindowAttribute(g_hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    if (!CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // 4. Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Disable dummy imgui.ini file generation!

    // Load Segoe UI Font with upward baseline adjustment to center text vertically
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 3;
    fontConfig.PixelSnapH = false;
    fontConfig.GlyphOffset.y = -1.0f; // Perfect vertical optical centering for Segoe UI
    
    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    FILE* f = nullptr;
    if (fopen_s(&f, fontPath, "r") == 0 && f != nullptr) {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(fontPath, 17.5f, &fontConfig, io.Fonts->GetGlyphRangesDefault());
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 5. Initialize UI Controller
    AppUI appUI(&muter, &config);
    appUI.Initialize();

    // Initialize System Tray Icon
    InitTrayIcon(g_hWnd);

    // Check Start Minimized
    std::wstring cmdLine(lpCmdLine ? lpCmdLine : L"");
    bool startMinimized = (cmdLine.find(L"--minimized") != std::wstring::npos) || config.startMinimized;

    if (startMinimized) {
        ShowWindow(g_hWnd, SW_HIDE);
        g_isWindowVisible = false;
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
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

            // If window was hidden during message processing (e.g. WM_CLOSE or minimize):
            if (!g_isWindowVisible) {
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
                continue;
            }

            if (appUI.ShouldMinimizeToTray()) {
                appUI.ResetMinimizeRequest();
                ShowWindow(g_hWnd, SW_HIDE);
                g_isWindowVisible = false;
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
                continue;
            }

            // Render ImGui Frame
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            appUI.Render();

            ImGui::Render();
            const float clear_color_with_alpha[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            g_pSwapChain->Present(1, 0); // V-Sync
        } else {
            // Suspended / Waiting in Tray (0.00% CPU usage & minimal RAM)
            BOOL bRet = GetMessage(&msg, NULL, 0, 0);
            if (bRet > 0) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } else {
                done = true;
            }
        }
    }

    // Cleanup
    RemoveTrayIcon();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    g_pGlobalConfig = nullptr;
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
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (g_pSwapChain) {
        HRESULT hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (SUCCEEDED(hr) && pBackBuffer) {
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hWnd, SW_HIDE);
            g_isWindowVisible = false;
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_MINIMIZE) {
            ShowWindow(hWnd, SW_HIDE);
            g_isWindowVisible = false;
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        g_isWindowVisible = false;
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            if (g_isWindowVisible) {
                ShowWindow(hWnd, SW_HIDE);
                g_isWindowVisible = false;
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            } else {
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
