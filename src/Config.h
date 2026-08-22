// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

#include "Utils.h"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")

struct AppConfig {
    bool isEnabled = false; // Master Guard Switch (disabled by default)
    std::wstring targetDeviceName = L"Sonar - Microphone";
    std::wstring targetProcessName = L""; // Empty by default (user selectable)
    bool startWithWindows = false;
    bool showNotifications = true;
    bool startMinimized = false;

    static std::wstring GetExecutablePath() {
        WCHAR path[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, path, MAX_PATH);
        return std::wstring(path);
    }

    static std::wstring GetConfigDirectory() {
        WCHAR appDataPath[MAX_PATH] = { 0 };
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
            std::wstring dir = std::wstring(appDataPath) + L"\\SonarCord";
            CreateDirectoryW(dir.c_str(), NULL);
            return dir;
        }

        // Fallback to exe directory
        std::wstring exePath = GetExecutablePath();
        size_t lastSlash = exePath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return exePath.substr(0, lastSlash);
        }
        return L".";
    }

    static std::wstring GetConfigPath() {
        return GetConfigDirectory() + L"\\config.ini";
    }

    void Load() {
        std::wstring configPath = GetConfigPath();
        
        isEnabled = GetPrivateProfileIntW(L"Settings", L"IsEnabled", 0, configPath.c_str()) != 0;

        WCHAR devName[256] = { 0 };
        GetPrivateProfileStringW(L"Settings", L"TargetDevice", L"Sonar - Microphone", devName, 256, configPath.c_str());
        targetDeviceName = devName;

        WCHAR procName[4096] = { 0 };
        GetPrivateProfileStringW(L"Settings", L"TargetProcess", L"", procName, 4096, configPath.c_str());
        targetProcessName = procName;

        startWithWindows = GetPrivateProfileIntW(L"Settings", L"StartWithWindows", 0, configPath.c_str()) != 0;
        showNotifications = GetPrivateProfileIntW(L"Settings", L"ShowNotifications", 1, configPath.c_str()) != 0;
        startMinimized = GetPrivateProfileIntW(L"Settings", L"StartMinimized", 0, configPath.c_str()) != 0;
    }

    bool Save() const {
        std::wstring configPath = GetConfigPath();

        BOOL ok = TRUE;
        ok &= WritePrivateProfileStringW(L"Settings", L"IsEnabled", isEnabled ? L"1" : L"0", configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"TargetDevice", targetDeviceName.c_str(), configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"TargetProcess", targetProcessName.c_str(), configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"StartWithWindows", startWithWindows ? L"1" : L"0", configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"ShowNotifications", showNotifications ? L"1" : L"0", configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"StartMinimized", startMinimized ? L"1" : L"0", configPath.c_str());
        return ok != FALSE;
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

    static bool SetAutoStart(bool enable) {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            LONG result;
            if (enable) {
                std::wstring exePath = L"\"" + GetExecutablePath() + L"\" --minimized";
                result = RegSetValueExW(hKey, L"SonarCord", 0, REG_SZ, (const BYTE*)exePath.c_str(), (DWORD)((exePath.length() + 1) * sizeof(WCHAR)));
            } else {
                result = RegDeleteValueW(hKey, L"SonarCord");
            }
            RegCloseKey(hKey);
            return (result == ERROR_SUCCESS);
        }
        return false;
    }
};
