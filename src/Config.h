// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <algorithm>

#include "Utils.h"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")

struct AppConfig {
    bool isEnabled = false; // Master Muter Switch (disabled by default)
    bool startWithWindows = false;
    bool showNotifications = true;
    bool startMinimized = false;
    bool unmuteOnExit = true;
    std::wstring lastSelectedDevice = L"";

    // Map: Device Friendly Name -> Vector of Process Names
    std::map<std::wstring, std::vector<std::wstring>> deviceTargets;

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
        startWithWindows = GetPrivateProfileIntW(L"Settings", L"StartWithWindows", 0, configPath.c_str()) != 0;
        showNotifications = GetPrivateProfileIntW(L"Settings", L"ShowNotifications", 1, configPath.c_str()) != 0;
        startMinimized = GetPrivateProfileIntW(L"Settings", L"StartMinimized", 0, configPath.c_str()) != 0;
        unmuteOnExit = GetPrivateProfileIntW(L"Settings", L"UnmuteOnExit", 1, configPath.c_str()) != 0;

        WCHAR lastDev[256] = { 0 };
        GetPrivateProfileStringW(L"Settings", L"LastSelectedDevice", L"", lastDev, 256, configPath.c_str());
        lastSelectedDevice = lastDev;

        deviceTargets.clear();

        // Read [DeviceTargets] Section
        WCHAR buffer[32768] = { 0 };
        DWORD charsRead = GetPrivateProfileSectionW(L"DeviceTargets", buffer, 32768, configPath.c_str());
        if (charsRead > 0) {
            const WCHAR* p = buffer;
            while (*p) {
                std::wstring line(p);
                size_t eqPos = line.find(L'=');
                if (eqPos != std::wstring::npos) {
                    std::wstring devName = line.substr(0, eqPos);
                    std::wstring procListStr = line.substr(eqPos + 1);

                    std::vector<std::wstring> procs;
                    std::wstringstream ss(procListStr);
                    std::wstring item;
                    while (std::getline(ss, item, L',')) {
                        item = Utils::Trim(item);
                        if (!item.empty()) {
                            procs.push_back(item);
                        }
                    }

                    if (!devName.empty() && !procs.empty()) {
                        deviceTargets[devName] = procs;
                    }
                }
                p += line.length() + 1;
            }
        }
    }

    bool Save() const {
        std::wstring configPath = GetConfigPath();

        BOOL ok = TRUE;
        ok &= WritePrivateProfileStringW(L"Settings", L"IsEnabled", isEnabled ? L"1" : L"0", configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"StartWithWindows", startWithWindows ? L"1" : L"0", configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"ShowNotifications", showNotifications ? L"1" : L"0", configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"StartMinimized", startMinimized ? L"1" : L"0", configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"UnmuteOnExit", unmuteOnExit ? L"1" : L"0", configPath.c_str());
        ok &= WritePrivateProfileStringW(L"Settings", L"LastSelectedDevice", lastSelectedDevice.c_str(), configPath.c_str());

        // Clear existing DeviceTargets section first to remove deleted devices
        WritePrivateProfileSectionW(L"DeviceTargets", L"", configPath.c_str());

        for (const auto& [devName, procs] : deviceTargets) {
            if (devName.empty() || procs.empty()) continue;
            std::wstring procsStr;
            for (size_t i = 0; i < procs.size(); ++i) {
                if (i > 0) procsStr += L",";
                procsStr += procs[i];
            }
            ok &= WritePrivateProfileStringW(L"DeviceTargets", devName.c_str(), procsStr.c_str(), configPath.c_str());
        }

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
