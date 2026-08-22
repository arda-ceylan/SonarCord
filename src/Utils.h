// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <windows.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cwctype>

namespace Utils {

inline std::string WStringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0) return "";
    std::string strTo(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);
    return strTo;
}

inline std::wstring Utf8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    if (sizeNeeded <= 0) return L"";
    std::wstring wstrTo(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], sizeNeeded);
    return wstrTo;
}

inline std::wstring ToLower(std::wstring str) {
    std::transform(str.begin(), str.end(), str.begin(), ::towlower);
    return str;
}

inline std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return (char)::tolower(c); });
    return str;
}

inline std::wstring Trim(const std::wstring& s) {
    auto start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    auto end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

inline std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

inline void TrimWorkingSet(HANDLE hProcess = GetCurrentProcess()) {
    SetProcessWorkingSetSize(hProcess, (SIZE_T)-1, (SIZE_T)-1);
}

inline std::wstring ExtractFileName(const std::wstring& fullPath) {
    size_t lastSlash = fullPath.find_last_of(L"\\/");
    return (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
}

// Case-insensitive exact or extension-omitted process matching
inline bool ProcessNamesMatch(const std::wstring& runningProc, const std::wstring& targetProc) {
    std::wstring rName = ToLower(ExtractFileName(Trim(runningProc)));
    std::wstring tName = ToLower(ExtractFileName(Trim(targetProc)));

    if (rName.empty() || tName.empty()) return false;

    // Exact match
    if (rName == tName) return true;

    // If target does not specify .exe extension, match stem
    if (tName.length() > 4 && tName.substr(tName.length() - 4) == L".exe") {
        return rName == tName;
    }

    // Check if runningProc equals target + ".exe"
    if (rName == tName + L".exe") {
        return true;
    }

    return false;
}

} // namespace Utils
