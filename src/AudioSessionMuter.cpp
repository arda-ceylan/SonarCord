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

    for (auto& [name, dev] : m_managedDevices) {
        DetachDeviceInternal(dev);
    }
    m_managedDevices.clear();

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
        ScanAndMuteAllManagedDevices();
    } else {
        AddLog("Muter paused - Restoring audio for target sessions.");
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

void AudioSessionMuter::AttachDeviceInternal(ManagedDevice& dev) {
    if (!m_pEnumerator || dev.friendlyName.empty()) return;

    std::wstring lowerTarget = Utils::ToLower(dev.friendlyName);

    // Enumerate active endpoints to get matching IMMDevice
    ComPtr<IMMDeviceCollection> pCollection;
    if (FAILED(m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection)) || !pCollection) {
        return;
    }

    UINT count = 0;
    pCollection->GetCount(&count);

    ComPtr<IMMDevice> pMatchedDevice;
    std::wstring matchedName;

    // Pass 1: Exact match
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> pDevice;
        if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
            ComPtr<IPropertyStore> pProps;
            if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                    std::wstring name = varName.pwszVal;
                    if (Utils::ToLower(name) == lowerTarget) {
                        pMatchedDevice = pDevice;
                        matchedName = name;
                        PropVariantClear(&varName);
                        break;
                    }
                }
                PropVariantClear(&varName);
            }
        }
    }

    // Pass 2: Substring match fallback
    if (!pMatchedDevice) {
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> pDevice;
            if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
                ComPtr<IPropertyStore> pProps;
                if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                        std::wstring name = varName.pwszVal;
                        if (Utils::ToLower(name).find(lowerTarget) != std::wstring::npos) {
                            pMatchedDevice = pDevice;
                            matchedName = name;
                            PropVariantClear(&varName);
                            break;
                        }
                    }
                    PropVariantClear(&varName);
                }
            }
        }
    }

    if (!pMatchedDevice) {
        dev.isConnected = false;
        return;
    }

    dev.pDevice = pMatchedDevice;
    HRESULT hr = dev.pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, reinterpret_cast<void**>(dev.pSessionManager.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !dev.pSessionManager) {
        dev.pDevice.Reset();
        dev.isConnected = false;
        return;
    }

    if (!dev.pNotificationHandler) {
        dev.pNotificationHandler = new AudioSessionNotificationHandler(this, dev.friendlyName);
        dev.pSessionManager->RegisterSessionNotification(dev.pNotificationHandler);
    }

    dev.isConnected = true;
    AddLog("Attached to device: " + Utils::WStringToUtf8(dev.friendlyName));

    if (m_isEnabled) {
        ScanAndMuteDevice(dev.friendlyName);
    }
}

void AudioSessionMuter::DetachDeviceInternal(ManagedDevice& dev) {
    if (dev.pSessionManager && dev.pNotificationHandler) {
        dev.pSessionManager->UnregisterSessionNotification(dev.pNotificationHandler);
        dev.pNotificationHandler->Release();
        dev.pNotificationHandler = nullptr;
    }
    dev.pSessionManager.Reset();
    dev.pDevice.Reset();
    dev.isConnected = false;
}

void AudioSessionMuter::SyncManagedDevicesWithSystem() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto sysDevs = EnumerateRenderDevices();

    for (auto& [name, dev] : m_managedDevices) {
        if (dev.targetProcesses.empty()) continue;

        bool foundInSystem = false;
        std::wstring lowerTarget = Utils::ToLower(name);
        for (const auto& s : sysDevs) {
            std::wstring lowerSys = Utils::ToLower(s.friendlyName);
            if (lowerSys == lowerTarget || lowerSys.find(lowerTarget) != std::wstring::npos) {
                foundInSystem = true;
                break;
            }
        }

        if (foundInSystem) {
            if (!dev.isConnected || !dev.pSessionManager) {
                AttachDeviceInternal(dev);
            }
        } else {
            if (dev.isConnected) {
                DetachDeviceInternal(dev);
                AddLog("Device offline: " + Utils::WStringToUtf8(name));
            }
        }
    }
}

void AudioSessionMuter::SetDeviceTargets(const std::wstring& devName, const std::vector<std::wstring>& targets) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (devName.empty()) return;

    if (targets.empty()) {
        auto it = m_managedDevices.find(devName);
        if (it != m_managedDevices.end()) {
            if (m_isEnabled && it->second.isConnected) {
                for (const auto& p : it->second.targetProcesses) {
                    UnmuteProcessForDevice(devName, p);
                }
            }
            DetachDeviceInternal(it->second);
            m_managedDevices.erase(it);
            AddLog("Removed device configuration: " + Utils::WStringToUtf8(devName));
        }
    } else {
        ManagedDevice& dev = m_managedDevices[devName];
        dev.friendlyName = devName;
        dev.targetProcesses = targets;

        if (!dev.isConnected || !dev.pSessionManager) {
            AttachDeviceInternal(dev);
        } else {
            if (m_isEnabled) {
                ScanAndMuteDevice(devName);
            }
        }
    }
}

void AudioSessionMuter::AddTargetProcess(const std::wstring& devName, const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (devName.empty() || procName.empty()) return;

    std::vector<std::wstring> targets = GetDeviceTargets(devName);
    for (const auto& t : targets) {
        if (Utils::ProcessNamesMatch(t, procName)) return; // Already exists
    }
    targets.push_back(procName);
    SetDeviceTargets(devName, targets);
}

void AudioSessionMuter::RemoveTargetProcess(const std::wstring& devName, const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (devName.empty() || procName.empty()) return;

    std::vector<std::wstring> targets = GetDeviceTargets(devName);
    auto it = std::remove_if(targets.begin(), targets.end(), [&](const std::wstring& item) {
        return Utils::ProcessNamesMatch(item, procName);
    });

    if (it != targets.end()) {
        targets.erase(it, targets.end());
        if (m_isEnabled) {
            UnmuteProcessForDevice(devName, procName);
        }
        SetDeviceTargets(devName, targets);
    }
}

std::vector<std::wstring> AudioSessionMuter::GetDeviceTargets(const std::wstring& devName) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_managedDevices.find(devName);
    if (it != m_managedDevices.end()) {
        return it->second.targetProcesses;
    }
    return {};
}

void AudioSessionMuter::SetAllDeviceTargets(const std::map<std::wstring, std::vector<std::wstring>>& configMap) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Remove devices not in configMap
    std::vector<std::wstring> toRemove;
    for (const auto& [name, dev] : m_managedDevices) {
        if (configMap.find(name) == configMap.end() || configMap.at(name).empty()) {
            toRemove.push_back(name);
        }
    }
    for (const auto& name : toRemove) {
        SetDeviceTargets(name, {});
    }

    // Add or update
    for (const auto& [name, targets] : configMap) {
        if (!targets.empty()) {
            SetDeviceTargets(name, targets);
        }
    }

    SyncManagedDevicesWithSystem();
}

bool AudioSessionMuter::IsDeviceManaged(const std::wstring& devName) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_managedDevices.find(devName);
    return (it != m_managedDevices.end() && !it->second.targetProcesses.empty());
}

bool AudioSessionMuter::IsDeviceConnected(const std::wstring& devName) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_managedDevices.find(devName);
    if (it != m_managedDevices.end()) {
        return it->second.isConnected && (it->second.pSessionManager != nullptr);
    }
    return false;
}

int AudioSessionMuter::GetManagedDeviceCount() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    int count = 0;
    for (const auto& [name, dev] : m_managedDevices) {
        if (!dev.targetProcesses.empty()) count++;
    }
    return count;
}

int AudioSessionMuter::GetConnectedManagedDeviceCount() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    int count = 0;
    for (const auto& [name, dev] : m_managedDevices) {
        if (!dev.targetProcesses.empty() && dev.isConnected && dev.pSessionManager) {
            count++;
        }
    }
    return count;
}

std::vector<std::wstring> AudioSessionMuter::GetManagedDeviceNames() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<std::wstring> names;
    for (const auto& [name, dev] : m_managedDevices) {
        if (!dev.targetProcesses.empty()) {
            names.push_back(name);
        }
    }
    return names;
}

std::vector<ActiveSessionInfo> AudioSessionMuter::GetActiveSessions(const std::wstring& devName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<ActiveSessionInfo> results;
    if (devName.empty() || !m_pEnumerator) return results;

    ComPtr<IAudioSessionManager2> pMgr;
    auto it = m_managedDevices.find(devName);
    if (it != m_managedDevices.end() && it->second.pSessionManager) {
        pMgr = it->second.pSessionManager;
    } else {
        // Fallback: If not managed yet or disconnected, find IMMDevice and activate temporarily to preview sessions
        std::wstring lowerTarget = Utils::ToLower(devName);
        ComPtr<IMMDeviceCollection> pCollection;
        if (SUCCEEDED(m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection)) && pCollection) {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> pDev;
                if (SUCCEEDED(pCollection->Item(i, &pDev))) {
                    ComPtr<IPropertyStore> pProps;
                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps))) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                            std::wstring name = varName.pwszVal;
                            if (Utils::ToLower(name) == lowerTarget || Utils::ToLower(name).find(lowerTarget) != std::wstring::npos) {
                                pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, reinterpret_cast<void**>(pMgr.ReleaseAndGetAddressOf()));
                                PropVariantClear(&varName);
                                break;
                            }
                        }
                        PropVariantClear(&varName);
                    }
                }
            }
        }
    }

    if (!pMgr) return results;

    std::vector<std::wstring> targets = GetDeviceTargets(devName);

    ComPtr<IAudioSessionEnumerator> pSessionEnum;
    if (SUCCEEDED(pMgr->GetSessionEnumerator(&pSessionEnum)) && pSessionEnum) {
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

                    for (const auto& target : targets) {
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

void AudioSessionMuter::ScanAndMuteAllManagedDevices() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_isEnabled) return;

    for (const auto& [name, dev] : m_managedDevices) {
        if (dev.isConnected && dev.pSessionManager) {
            ScanAndMuteDevice(name);
        }
    }
}

void AudioSessionMuter::ScanAndMuteDevice(const std::wstring& devName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_isEnabled) return;

    auto it = m_managedDevices.find(devName);
    if (it == m_managedDevices.end() || !it->second.isConnected || !it->second.pSessionManager) return;

    ComPtr<IAudioSessionEnumerator> pSessionEnum;
    HRESULT hr = it->second.pSessionManager->GetSessionEnumerator(&pSessionEnum);
    if (FAILED(hr) || !pSessionEnum) {
        // Zombie COM session manager recovery (driver reload/crash)
        DetachDeviceInternal(it->second);
        AddLog("Device connection lost: " + Utils::WStringToUtf8(devName));
        return;
    }

    int sessionCount = 0;
    pSessionEnum->GetCount(&sessionCount);
    for (int i = 0; i < sessionCount; ++i) {
        ComPtr<IAudioSessionControl> pSessionControl;
        if (SUCCEEDED(pSessionEnum->GetSession(i, &pSessionControl)) && pSessionControl) {
            ProcessSessionForDevice(devName, pSessionControl.Get());
        }
    }
}

void AudioSessionMuter::UnmuteProcessForDevice(const std::wstring& devName, const std::wstring& procName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_managedDevices.find(devName);
    if (it == m_managedDevices.end() || !it->second.pSessionManager) return;

    ComPtr<IAudioSessionEnumerator> pSessionEnum;
    HRESULT hr = it->second.pSessionManager->GetSessionEnumerator(&pSessionEnum);
    if (FAILED(hr) || !pSessionEnum) {
        DetachDeviceInternal(it->second);
        return;
    }

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
                    std::wstring actualName;
                    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (hProcess) {
                        WCHAR processPath[MAX_PATH];
                        DWORD size = MAX_PATH;
                        if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
                            actualName = Utils::ExtractFileName(processPath);
                        }
                        CloseHandle(hProcess);
                    }

                    if (Utils::ProcessNamesMatch(actualName, procName)) {
                        ComPtr<ISimpleAudioVolume> pVolume;
                        if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pVolume))) && pVolume) {
                            BOOL isMuted = FALSE;
                            pVolume->GetMute(&isMuted);
                            if (isMuted) {
                                pVolume->SetMute(FALSE, NULL);
                                std::string msg = "UNMUTED: " + Utils::WStringToUtf8(actualName) + " on " + Utils::WStringToUtf8(devName);
                                AddLog(msg, true);
                            }
                        }
                    }
                }
            }
        }
    }
}

void AudioSessionMuter::UnmuteAllTargets() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& [devName, dev] : m_managedDevices) {
        for (const auto& procName : dev.targetProcesses) {
            UnmuteProcessForDevice(devName, procName);
        }
    }
}

void AudioSessionMuter::ProcessSessionForDevice(const std::wstring& devName, IAudioSessionControl* pSessionControl) {
    if (!pSessionControl) return;

    std::function<void(const std::wstring&, const std::wstring&, DWORD)> callbackToInvoke;
    std::wstring matchedName;
    DWORD matchedPid = 0;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_isEnabled) return;

        auto it = m_managedDevices.find(devName);
        if (it == m_managedDevices.end() || !it->second.isConnected) return;

        ComPtr<IAudioSessionControl2> pSessionControl2;
        if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pSessionControl2))) && pSessionControl2) {
            DWORD pid = 0;
            pSessionControl2->GetProcessId(&pid);

            std::wstring actualProcName;
            if (IsMatchingProcess(pid, it->second.targetProcesses, actualProcName)) {
                ComPtr<ISimpleAudioVolume> pVolume;
                if (SUCCEEDED(pSessionControl->QueryInterface(IID_PPV_ARGS(&pVolume))) && pVolume) {
                    BOOL isMuted = FALSE;
                    pVolume->GetMute(&isMuted);
                    if (!isMuted) {
                        pVolume->SetMute(TRUE, NULL);
                        std::string msg = "MUTED: " + Utils::WStringToUtf8(actualProcName) + " on " + Utils::WStringToUtf8(devName) + " (PID: " + std::to_string(pid) + ")";
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
        callbackToInvoke(devName, matchedName, matchedPid);
    }
}

bool AudioSessionMuter::IsMatchingProcess(DWORD pid, const std::vector<std::wstring>& targets, std::wstring& outProcName) {
    if (pid == 0 || targets.empty()) return false;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;

    WCHAR processPath[MAX_PATH];
    DWORD size = MAX_PATH;
    bool matched = false;

    if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
        std::wstring fileName = Utils::ExtractFileName(processPath);
        for (const auto& target : targets) {
            if (Utils::ProcessNamesMatch(fileName, target)) {
                outProcName = fileName;
                matched = true;
                break;
            }
        }
    }

    CloseHandle(hProcess);
    return matched;
}

void AudioSessionMuter::OnAudioEndpointsChanged() {
    std::function<void()> devCallback;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        devCallback = m_onDeviceChangedCallback;
    }

    if (devCallback) {
        devCallback();
    }
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
        m_logs.pop_front();
    }
    m_logRevision++;
}

std::vector<LogEntry> AudioSessionMuter::GetLogs() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return std::vector<LogEntry>(m_logs.begin(), m_logs.end());
}

uint64_t AudioSessionMuter::GetLogRevision() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_logRevision;
}

void AudioSessionMuter::ClearLogs() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_logs.clear();
    m_logRevision++;
}

HRESULT STDMETHODCALLTYPE AudioSessionNotificationHandler::OnSessionCreated(IAudioSessionControl* NewSession) {
    if (m_pParent) {
        m_pParent->ProcessSessionForDevice(m_deviceName, NewSession);
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
