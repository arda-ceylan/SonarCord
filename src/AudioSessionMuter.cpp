// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#include "AudioSessionMuter.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>

AudioSessionMuter::AudioSessionMuter() {
}

AudioSessionMuter::~AudioSessionMuter() {
    Shutdown();
}

bool AudioSessionMuter::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        m_comInitialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        m_comInitialized = false; // COM was initialized elsewhere in MTA mode
    } else {
        AddLog("ERROR: Failed to initialize COM!");
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&m_pEnumerator));
    if (FAILED(hr) || !m_pEnumerator) {
        AddLog("ERROR: Failed to create MMDeviceEnumerator!");
        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
        return false;
    }

    // Register audio endpoint listener (Detects SteelSeries Sonar / hotplugged devices when Windows boots)
    if (!m_pEndpointHandler) {
        m_pEndpointHandler = new AudioEndpointNotificationHandler(this);
        m_pEnumerator->RegisterEndpointNotificationCallback(m_pEndpointHandler);
    }

    AddLog("Audio controller initialized.");
    return true;
}

void AudioSessionMuter::Shutdown(bool unmuteOnExit) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (unmuteOnExit) {
        UnmuteAllTargets();
    }
    Detach();

    if (m_pEnumerator && m_pEndpointHandler) {
        m_pEnumerator->UnregisterEndpointNotificationCallback(m_pEndpointHandler);
        m_pEndpointHandler->Release();
        m_pEndpointHandler = nullptr;
    }

    m_pEnumerator.Reset();

    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

void AudioSessionMuter::SetEnabled(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_isEnabled == enable) return;
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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

bool AudioSessionMuter::AttachToDevice(const std::wstring& targetNameOrSubstring, bool force) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::wstring lowerTarget = Utils::ToLower(targetNameOrSubstring);
    std::wstring lowerCurrent = Utils::ToLower(m_activeDeviceName);

    // If already attached to requested device and manager is alive, avoid duplicate detachment & log spam
    if (!force && m_isAttached && m_pSessionManager && !m_activeDeviceName.empty()) {
        if (lowerCurrent.find(lowerTarget) != std::wstring::npos) {
            return true;
        }
    }

    Detach();

    m_lastRequestedDevice = targetNameOrSubstring;

    if (!m_pEnumerator) return false;

    ComPtr<IMMDeviceCollection> pCollection;
    if (FAILED(m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection)) || !pCollection) {
        return false;
    }

    UINT count = 0;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> pDevice;
        if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
            ComPtr<IPropertyStore> pProps;
            if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                    std::wstring name = varName.pwszVal;
                    std::wstring lowerName = Utils::ToLower(name);

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
        AddLog("Device not found: " + Utils::WStringToUtf8(targetNameOrSubstring));
        m_isAttached = false;
        return false;
    }

    // AudioSessionManager2
    HRESULT hr = m_pCurrentDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)m_pSessionManager.ReleaseAndGetAddressOf());
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
    AddLog("Attached to device: " + Utils::WStringToUtf8(m_activeDeviceName));

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
    std::function<void()> devCallback;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        devCallback = m_onDeviceChangedCallback;
    }

    if (devCallback) {
        devCallback();
    } else {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_isAttached && !m_lastRequestedDevice.empty()) {
            AttachToDevice(m_lastRequestedDevice);
        }
    }
}

void AudioSessionMuter::SetTargetProcess(const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_targetProcesses.clear();
    
    std::wstringstream ss(procName);
    std::wstring item;
    while (std::getline(ss, item, L'|')) {
        std::wstring trimmed = Utils::Trim(item);
        if (!trimmed.empty()) {
            m_targetProcesses.push_back(trimmed);
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
        if (i + 1 < m_targetProcesses.size()) ss << L"|";
    }
    return ss.str();
}

void AudioSessionMuter::SetTargetProcessList(const std::vector<std::wstring>& list) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_targetProcesses.clear();
    for (const auto& item : list) {
        std::wstring trimmed = Utils::Trim(item);
        if (!trimmed.empty()) {
            m_targetProcesses.push_back(trimmed);
        }
    }
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
    std::wstring trimmed = Utils::Trim(procName);
    if (trimmed.empty()) return;

    for (const auto& item : m_targetProcesses) {
        if (Utils::ProcessNamesMatch(item, trimmed)) return;
    }

    m_targetProcesses.push_back(trimmed);
    AddLog("Added to target list: " + Utils::WStringToUtf8(trimmed));
    if (m_isEnabled) {
        ScanAndMuteExistingSessions();
    }
}

void AudioSessionMuter::RemoveTargetProcess(const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstring trimmed = Utils::Trim(procName);
    if (trimmed.empty()) return;

    auto it = std::remove_if(m_targetProcesses.begin(), m_targetProcesses.end(), [&](const std::wstring& item) {
        return Utils::ProcessNamesMatch(item, trimmed);
    });

    if (it != m_targetProcesses.end()) {
        m_targetProcesses.erase(it, m_targetProcesses.end());
        AddLog("Removed from target list: " + Utils::WStringToUtf8(trimmed));
        UnmuteProcess(trimmed);
    }
}

void AudioSessionMuter::UnmuteProcess(const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pSessionManager) return;

    ComPtr<IAudioSessionEnumerator> pSessionEnum;
    if (SUCCEEDED(m_pSessionManager->GetSessionEnumerator(&pSessionEnum)) && pSessionEnum) {
        int sessionCount = 0;
        pSessionEnum->GetCount(&sessionCount);
        for (int i = 0; i < sessionCount; ++i) {
            ComPtr<IAudioSessionControl> pSessionControl;
            if (SUCCEEDED(pSessionEnum->GetSession(i, &pSessionControl)) && pSessionControl) {
                ComPtr<IAudioSessionControl2> pSessionControl2;
                if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pSessionControl2))) && pSessionControl2) {
                    DWORD pid = 0;
                    pSessionControl2->GetProcessId(&pid);
                    if (pid != 0) {
                        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        if (hProcess) {
                            WCHAR processPath[MAX_PATH];
                            DWORD size = MAX_PATH;
                            if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
                                std::wstring actualProcName = Utils::ExtractFileName(processPath);

                                if (Utils::ProcessNamesMatch(actualProcName, procName)) {
                                    ComPtr<ISimpleAudioVolume> pVolume;
                                    if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pVolume))) && pVolume) {
                                        pVolume->SetMute(FALSE, NULL);
                                        AddLog("UNMUTED: " + Utils::WStringToUtf8(actualProcName) + " (PID: " + std::to_string(pid) + ")", true);
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pSessionManager) return;

    for (const auto& target : m_targetProcesses) {
        UnmuteProcess(target);
    }
}

bool AudioSessionMuter::IsAttached() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
            if (SUCCEEDED(pSessionEnum->GetSession(i, &pSessionControl)) && pSessionControl) {
                ComPtr<IAudioSessionControl2> pSessionControl2;
                if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pSessionControl2))) && pSessionControl2) {
                    DWORD pid = 0;
                    pSessionControl2->GetProcessId(&pid);

                    std::wstring procName = L"System/Unknown";
                    if (pid != 0) {
                        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        if (hProcess) {
                            WCHAR processPath[MAX_PATH];
                            DWORD size = MAX_PATH;
                            if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
                                procName = Utils::ExtractFileName(processPath);
                            }
                            CloseHandle(hProcess);
                        }
                    }

                    BOOL isMuted = FALSE;
                    ComPtr<ISimpleAudioVolume> pVolume;
                    if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pVolume))) && pVolume) {
                        pVolume->GetMute(&isMuted);
                    }

                    ActiveSessionInfo info;
                    info.pid = pid;
                    info.processName = procName;
                    info.isMuted = (isMuted != FALSE);

                    for (const auto& target : m_targetProcesses) {
                        if (Utils::ProcessNamesMatch(procName, target)) {
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_isEnabled || !m_pSessionManager) return;

    ComPtr<IAudioSessionEnumerator> pSessionEnum;
    if (SUCCEEDED(m_pSessionManager->GetSessionEnumerator(&pSessionEnum)) && pSessionEnum) {
        int sessionCount = 0;
        pSessionEnum->GetCount(&sessionCount);
        for (int i = 0; i < sessionCount; ++i) {
            ComPtr<IAudioSessionControl> pSessionControl;
            if (SUCCEEDED(pSessionEnum->GetSession(i, &pSessionControl)) && pSessionControl) {
                ProcessSession(pSessionControl.Get());
            }
        }
    }
}

void AudioSessionMuter::ProcessSession(IAudioSessionControl* pSessionControl) {
    if (!pSessionControl) return;

    std::function<void(const std::wstring&, DWORD)> callbackToInvoke;
    std::wstring matchedName;
    DWORD matchedPid = 0;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_isEnabled) return;

        ComPtr<IAudioSessionControl2> pSessionControl2;
        if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pSessionControl2))) && pSessionControl2) {
            DWORD pid = 0;
            pSessionControl2->GetProcessId(&pid);

            std::wstring actualProcName;
            if (IsMatchingProcess(pid, actualProcName)) {
                ComPtr<ISimpleAudioVolume> pVolume;
                if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pVolume))) && pVolume) {
                    BOOL isMuted = FALSE;
                    pVolume->GetMute(&isMuted);
                    if (!isMuted) {
                        pVolume->SetMute(TRUE, NULL);
                        std::string msg = "MUTED: " + Utils::WStringToUtf8(actualProcName) + " (PID: " + std::to_string(pid) + ")";
                        AddLog(msg, true);

                        matchedName = actualProcName;
                        matchedPid = pid;
                        callbackToInvoke = m_onMutedCallback;
                    }
                }
            }
        }
    }

    if (callbackToInvoke && matchedPid != 0) {
        callbackToInvoke(matchedName, matchedPid);
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
        outProcName = Utils::ExtractFileName(processPath);

        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (const auto& item : m_targetProcesses) {
            if (Utils::ProcessNamesMatch(outProcName, item)) {
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

    m_logs.push_back(std::move(entry));
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
