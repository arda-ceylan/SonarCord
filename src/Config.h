// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

struct AppConfig {
    bool isEnabled = false; // Master Guard Switch (disabled by default)
    std::wstring targetDeviceName = L"Sonar - Microphone";
    std::wstring targetProcessName = L""; // Empty by default (no hardcoded Discord)
    bool startWithWindows = false;
    bool showNotifications = true;
    bool startMinimized = false;

    static std::wstring GetExecutablePath() {
        WCHAR path[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, path, MAX_PATH);
        return std::wstring(path);
    }

    static std::wstring GetConfigPath() {
        std::wstring exePath = GetExecutablePath();
        size_t lastSlash = exePath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return exePath.substr(0, lastSlash + 1) + L"config.ini";
        }
        return L"config.ini";
    }

    void Load() {
        std::wstring configPath = GetConfigPath();
        
        isEnabled = GetPrivateProfileIntW(L"Settings", L"IsEnabled", 0, configPath.c_str()) != 0;

        WCHAR devName[256] = { 0 };
        GetPrivateProfileStringW(L"Settings", L"TargetDevice", L"Sonar - Microphone", devName, 256, configPath.c_str());
        targetDeviceName = devName;

        WCHAR procName[512] = { 0 };
        GetPrivateProfileStringW(L"Settings", L"TargetProcess", L"", procName, 512, configPath.c_str());
        targetProcessName = procName;

        startWithWindows = GetPrivateProfileIntW(L"Settings", L"StartWithWindows", 0, configPath.c_str()) != 0;
        showNotifications = GetPrivateProfileIntW(L"Settings", L"ShowNotifications", 1, configPath.c_str()) != 0;
        startMinimized = GetPrivateProfileIntW(L"Settings", L"StartMinimized", 0, configPath.c_str()) != 0;
    }

    void Save() const {
        std::wstring configPath = GetConfigPath();

        WritePrivateProfileStringW(L"Settings", L"IsEnabled", isEnabled ? L"1" : L"0", configPath.c_str());
        WritePrivateProfileStringW(L"Settings", L"TargetDevice", targetDeviceName.c_str(), configPath.c_str());
        WritePrivateProfileStringW(L"Settings", L"TargetProcess", targetProcessName.c_str(), configPath.c_str());
        WritePrivateProfileStringW(L"Settings", L"StartWithWindows", startWithWindows ? L"1" : L"0", configPath.c_str());
        WritePrivateProfileStringW(L"Settings", L"ShowNotifications", showNotifications ? L"1" : L"0", configPath.c_str());
        WritePrivateProfileStringW(L"Settings", L"StartMinimized", startMinimized ? L"1" : L"0", configPath.c_str());
    }

    static bool IsAutoStartEnabled() {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            WCHAR buffer[MAX_PATH] = { 0 };
            DWORD size = sizeof(buffer);
            DWORD type = REG_SZ;
            LONG result = RegQueryValueExW(hKey, L"SonarCord", NULL, &type, (LPBYTE)buffer, &size);
            RegCloseKey(hKey);
            return (result == ERROR_SUCCESS);
        }
        return false;
    }

    static void SetAutoStart(bool enable) {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            if (enable) {
                std::wstring exePath = L"\"" + GetExecutablePath() + L"\" --minimized";
                RegSetValueExW(hKey, L"SonarCord", 0, REG_SZ, (const BYTE*)exePath.c_str(), (DWORD)((exePath.length() + 1) * sizeof(WCHAR)));
            } else {
                RegDeleteValueW(hKey, L"SonarCord");
            }
            RegCloseKey(hKey);
        }
    }
};
