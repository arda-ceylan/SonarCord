// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#include "AudioSessionMuter.h"
#include <psapi.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>

static std::string WStringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);
    return strTo;
}

AudioSessionMuter::AudioSessionMuter() {
}

AudioSessionMuter::~AudioSessionMuter() {
    Shutdown();
}

bool AudioSessionMuter::Initialize() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        AddLog("ERROR: Failed to initialize COM!");
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&m_pEnumerator));
    if (FAILED(hr)) {
        AddLog("ERROR: Failed to create MMDeviceEnumerator!");
        return false;
    }

    // Register audio endpoint listener (Detects SteelSeries Sonar / hotplugged devices when Windows boots)
    m_pEndpointHandler = new AudioEndpointNotificationHandler(this);
    m_pEnumerator->RegisterEndpointNotificationCallback(m_pEndpointHandler);

    AddLog("Audio controller initialized.");
    return true;
}

void AudioSessionMuter::Shutdown() {
    Detach();
    if (m_pEnumerator && m_pEndpointHandler) {
        m_pEnumerator->UnregisterEndpointNotificationCallback(m_pEndpointHandler);
        m_pEndpointHandler->Release();
        m_pEndpointHandler = nullptr;
    }
    m_pEnumerator.Reset();
    CoUninitialize();
}

void AudioSessionMuter::SetEnabled(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_isEnabled = enable;
    if (m_isEnabled) {
        AddLog("Muter enabled - Watching audio sessions.");
        ScanAndMuteExistingSessions();
    } else {
        AddLog("Muter paused - Unmuting all target audio sessions.");
        UnmuteAllTargets();
    }
}

bool AudioSessionMuter::IsEnabled() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_isEnabled;
}

std::vector<AudioDeviceInfo> AudioSessionMuter::EnumerateRenderDevices() {
    std::vector<AudioDeviceInfo> devices;
    if (!m_pEnumerator) return devices;

    ComPtr<IMMDeviceCollection> pCollection;
    HRESULT hr = m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr) || !pCollection) return devices;

    UINT count = 0;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> pDevice;
        if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
            LPWSTR pstrId = nullptr;
            pDevice->GetId(&pstrId);

            ComPtr<IPropertyStore> pProps;
            std::wstring friendlyName = L"Unknown Device";
            if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                    friendlyName = varName.pwszVal;
                }
                PropVariantClear(&varName);
            }

            AudioDeviceInfo info;
            info.id = pstrId ? pstrId : L"";
            info.friendlyName = friendlyName;
            devices.push_back(info);

            if (pstrId) CoTaskMemFree(pstrId);
        }
    }
    return devices;
}

bool AudioSessionMuter::AttachToDevice(const std::wstring& targetNameOrSubstring) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Detach();

    m_lastRequestedDevice = targetNameOrSubstring;

    if (!m_pEnumerator) return false;

    ComPtr<IMMDeviceCollection> pCollection;
    if (FAILED(m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection))) {
        return false;
    }

    UINT count = 0;
    pCollection->GetCount(&count);

    std::wstring lowerTarget = targetNameOrSubstring;
    std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::towlower);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> pDevice;
        if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
            ComPtr<IPropertyStore> pProps;
            if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                    std::wstring name = varName.pwszVal;
                    std::wstring lowerName = name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);

                    if (lowerName.find(lowerTarget) != std::wstring::npos) {
                        m_pCurrentDevice = pDevice;
                        m_activeDeviceName = name;
                        PropVariantClear(&varName);
                        break;
                    }
                }
                PropVariantClear(&varName);
            }
        }
    }

    if (!m_pCurrentDevice) {
        AddLog("Device not found: " + WStringToUtf8(targetNameOrSubstring));
        m_isAttached = false;
        return false;
    }

    // AudioSessionManager2
    HRESULT hr = m_pCurrentDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&m_pSessionManager);
    if (FAILED(hr) || !m_pSessionManager) {
        AddLog("ERROR: Failed to obtain SessionManager2!");
        m_pCurrentDevice.Reset();
        m_isAttached = false;
        return false;
    }

    // Attach Event Handler
    m_pNotificationHandler = new AudioSessionNotificationHandler(this);
    m_pSessionManager->RegisterSessionNotification(m_pNotificationHandler);

    m_isAttached = true;
    AddLog("Attached to device: " + WStringToUtf8(m_activeDeviceName));

    if (m_isEnabled) {
        ScanAndMuteExistingSessions();
    }
    return true;
}

void AudioSessionMuter::Detach() {
    if (m_pSessionManager && m_pNotificationHandler) {
        m_pSessionManager->UnregisterSessionNotification(m_pNotificationHandler);
        m_pNotificationHandler->Release();
        m_pNotificationHandler = nullptr;
    }
    m_pSessionManager.Reset();
    m_pCurrentDevice.Reset();
    m_activeDeviceName = L"";
    m_isAttached = false;
}

void AudioSessionMuter::OnAudioEndpointsChanged() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // If not currently attached, try auto-attaching to our target device!
    if (!m_isAttached && !m_lastRequestedDevice.empty()) {
        AttachToDevice(m_lastRequestedDevice);
    }
}

void AudioSessionMuter::SetTargetProcess(const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_targetProcesses.clear();
    std::wstringstream ss(procName);
    std::wstring item;
    while (std::getline(ss, item, L',')) {
        size_t start = item.find_first_not_of(L" \t");
        size_t end = item.find_last_not_of(L" \t");
        if (start != std::wstring::npos && end != std::wstring::npos) {
            m_targetProcesses.push_back(item.substr(start, end - start + 1));
        }
    }
    if (m_isEnabled) {
        ScanAndMuteExistingSessions();
    }
}

std::wstring AudioSessionMuter::GetTargetProcess() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstringstream ss;
    for (size_t i = 0; i < m_targetProcesses.size(); ++i) {
        ss << m_targetProcesses[i];
        if (i + 1 < m_targetProcesses.size()) ss << L", ";
    }
    return ss.str();
}

void AudioSessionMuter::SetTargetProcessList(const std::vector<std::wstring>& list) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_targetProcesses = list;
    if (m_isEnabled) {
        ScanAndMuteExistingSessions();
    }
}

std::vector<std::wstring> AudioSessionMuter::GetTargetProcessList() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_targetProcesses;
}

void AudioSessionMuter::AddTargetProcess(const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstring lowerProc = procName;
    std::transform(lowerProc.begin(), lowerProc.end(), lowerProc.begin(), ::towlower);

    for (const auto& item : m_targetProcesses) {
        std::wstring lowerItem = item;
        std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(), ::towlower);
        if (lowerItem == lowerProc) return;
    }

    m_targetProcesses.push_back(procName);
    AddLog("Added to target list: " + WStringToUtf8(procName));
    if (m_isEnabled) {
        ScanAndMuteExistingSessions();
    }
}

void AudioSessionMuter::RemoveTargetProcess(const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstring lowerProc = procName;
    std::transform(lowerProc.begin(), lowerProc.end(), lowerProc.begin(), ::towlower);

    auto it = std::remove_if(m_targetProcesses.begin(), m_targetProcesses.end(), [&](const std::wstring& item) {
        std::wstring lowerItem = item;
        std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(), ::towlower);
        return lowerItem == lowerProc;
    });

    if (it != m_targetProcesses.end()) {
        m_targetProcesses.erase(it, m_targetProcesses.end());
        AddLog("Removed from target list: " + WStringToUtf8(procName));
        UnmuteProcess(procName);
    }
}

void AudioSessionMuter::UnmuteProcess(const std::wstring& procName) {
    if (!m_pSessionManager) return;

    std::wstring lowerProc = procName;
    std::transform(lowerProc.begin(), lowerProc.end(), lowerProc.begin(), ::towlower);

    ComPtr<IAudioSessionEnumerator> pSessionEnum;
    if (SUCCEEDED(m_pSessionManager->GetSessionEnumerator(&pSessionEnum)) && pSessionEnum) {
        int sessionCount = 0;
        pSessionEnum->GetCount(&sessionCount);
        for (int i = 0; i < sessionCount; ++i) {
            ComPtr<IAudioSessionControl> pSessionControl;
            if (SUCCEEDED(pSessionEnum->GetSession(i, &pSessionControl))) {
                ComPtr<IAudioSessionControl2> pSessionControl2;
                if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pSessionControl2)))) {
                    DWORD pid = 0;
                    pSessionControl2->GetProcessId(&pid);
                    if (pid != 0) {
                        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        if (hProcess) {
                            WCHAR processPath[MAX_PATH];
                            DWORD size = MAX_PATH;
                            if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
                                std::wstring fullPath(processPath);
                                size_t lastSlash = fullPath.find_last_of(L"\\/");
                                std::wstring actualProcName = (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
                                
                                std::wstring lowerFound = actualProcName;
                                std::transform(lowerFound.begin(), lowerFound.end(), lowerFound.begin(), ::towlower);

                                if (lowerFound.find(lowerProc) != std::wstring::npos) {
                                    ComPtr<ISimpleAudioVolume> pVolume;
                                    if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pVolume)))) {
                                        pVolume->SetMute(FALSE, NULL);
                                        AddLog("UNMUTED: " + WStringToUtf8(actualProcName) + " (PID: " + std::to_string(pid) + ")", true);
                                    }
                                }
                            }
                            CloseHandle(hProcess);
                        }
                    }
                }
            }
        }
    }
}

void AudioSessionMuter::UnmuteAllTargets() {
    if (!m_pSessionManager) return;

    for (const auto& target : m_targetProcesses) {
        UnmuteProcess(target);
    }
}

bool AudioSessionMuter::IsAttached() const {
    return m_isAttached;
}

std::wstring AudioSessionMuter::GetActiveDeviceName() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_activeDeviceName;
}

std::vector<ActiveSessionInfo> AudioSessionMuter::GetActiveSessions() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<ActiveSessionInfo> results;
    if (!m_pSessionManager) return results;

    ComPtr<IAudioSessionEnumerator> pSessionEnum;
    if (SUCCEEDED(m_pSessionManager->GetSessionEnumerator(&pSessionEnum)) && pSessionEnum) {
        int sessionCount = 0;
        pSessionEnum->GetCount(&sessionCount);
        for (int i = 0; i < sessionCount; ++i) {
            ComPtr<IAudioSessionControl> pSessionControl;
            if (SUCCEEDED(pSessionEnum->GetSession(i, &pSessionControl))) {
                ComPtr<IAudioSessionControl2> pSessionControl2;
                if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pSessionControl2)))) {
                    DWORD pid = 0;
                    pSessionControl2->GetProcessId(&pid);

                    std::wstring procName = L"System/Unknown";
                    if (pid != 0) {
                        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        if (hProcess) {
                            WCHAR processPath[MAX_PATH];
                            DWORD size = MAX_PATH;
                            if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
                                std::wstring fullPath(processPath);
                                size_t lastSlash = fullPath.find_last_of(L"\\/");
                                procName = (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
                            }
                            CloseHandle(hProcess);
                        }
                    }

                    BOOL isMuted = FALSE;
                    ComPtr<ISimpleAudioVolume> pVolume;
                    if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pVolume)))) {
                        pVolume->GetMute(&isMuted);
                    }

                    ActiveSessionInfo info;
                    info.pid = pid;
                    info.processName = procName;
                    info.isMuted = (isMuted != FALSE);

                    std::wstring lowerFound = procName;
                    std::transform(lowerFound.begin(), lowerFound.end(), lowerFound.begin(), ::towlower);
                    for (const auto& target : m_targetProcesses) {
                        std::wstring lowerTarget = target;
                        std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::towlower);
                        if (lowerFound.find(lowerTarget) != std::wstring::npos) {
                            info.isTarget = true;
                            break;
                        }
                    }

                    results.push_back(info);
                }
            }
        }
    }
    return results;
}

void AudioSessionMuter::ScanAndMuteExistingSessions() {
    if (!m_isEnabled || !m_pSessionManager) return;

    ComPtr<IAudioSessionEnumerator> pSessionEnum;
    if (SUCCEEDED(m_pSessionManager->GetSessionEnumerator(&pSessionEnum)) && pSessionEnum) {
        int sessionCount = 0;
        pSessionEnum->GetCount(&sessionCount);
        for (int i = 0; i < sessionCount; ++i) {
            ComPtr<IAudioSessionControl> pSessionControl;
            if (SUCCEEDED(pSessionEnum->GetSession(i, &pSessionControl))) {
                ProcessSession(pSessionControl.Get());
            }
        }
    }
}

void AudioSessionMuter::ProcessSession(IAudioSessionControl* pSessionControl) {
    if (!pSessionControl || !m_isEnabled) return;

    ComPtr<IAudioSessionControl2> pSessionControl2;
    if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pSessionControl2)))) {
        DWORD pid = 0;
        pSessionControl2->GetProcessId(&pid);

        std::wstring actualProcName;
        if (IsMatchingProcess(pid, actualProcName)) {
            ComPtr<ISimpleAudioVolume> pVolume;
            if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pVolume)))) {
                BOOL isMuted = FALSE;
                pVolume->GetMute(&isMuted);
                if (!isMuted) {
                    pVolume->SetMute(TRUE, NULL);
                    std::string msg = "MUTED: " + WStringToUtf8(actualProcName) + " (PID: " + std::to_string(pid) + ")";
                    AddLog(msg, true);

                    if (m_onMutedCallback) {
                        m_onMutedCallback(actualProcName, pid);
                    }
                }
            }
        }
    }
}

bool AudioSessionMuter::IsMatchingProcess(DWORD pid, std::wstring& outProcName) {
    if (pid == 0) return false;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;

    WCHAR processPath[MAX_PATH];
    DWORD size = MAX_PATH;
    bool match = false;

    if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
        std::wstring fullPath(processPath);
        size_t lastSlash = fullPath.find_last_of(L"\\/");
        outProcName = (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;

        std::wstring lowerFound = outProcName;
        std::transform(lowerFound.begin(), lowerFound.end(), lowerFound.begin(), ::towlower);

        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (const auto& item : m_targetProcesses) {
            std::wstring lowerTarget = item;
            std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::towlower);

            if (lowerFound.find(lowerTarget) != std::wstring::npos) {
                match = true;
                break;
            }
        }
    }
    CloseHandle(hProcess);
    return match;
}

std::string AudioSessionMuter::GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    struct tm buf;
    localtime_s(&buf, &in_time_t);
    ss << std::put_time(&buf, "%H:%M:%S");
    return ss.str();
}

void AudioSessionMuter::AddLog(const std::string& message, bool isAction) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LogEntry entry;
    entry.timestamp = GetCurrentTimestamp();
    entry.message = message;
    entry.isAction = isAction;

    m_logs.push_back(entry);
    if (m_logs.size() > 200) {
        m_logs.erase(m_logs.begin());
    }
}

std::vector<LogEntry> AudioSessionMuter::GetLogs() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_logs;
}

void AudioSessionMuter::ClearLogs() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_logs.clear();
}

HRESULT STDMETHODCALLTYPE AudioSessionNotificationHandler::OnSessionCreated(IAudioSessionControl* NewSession) {
    if (m_pParent) {
        m_pParent->ProcessSession(NewSession);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioEndpointNotificationHandler::OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) {
    if (m_pParent) {
        m_pParent->OnAudioEndpointsChanged();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioEndpointNotificationHandler::OnDeviceAdded(LPCWSTR pwstrDeviceId) {
    if (m_pParent) {
        m_pParent->OnAudioEndpointsChanged();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioEndpointNotificationHandler::OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
    if (m_pParent) {
        m_pParent->OnAudioEndpointsChanged();
    }
    return S_OK;
}
