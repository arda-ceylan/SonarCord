// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <functional>

#include "Utils.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Psapi.lib")

using Microsoft::WRL::ComPtr;

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring friendlyName;
};

struct ActiveSessionInfo {
    DWORD pid = 0;
    std::wstring processName;
    bool isMuted = false;
    bool isTarget = false;
};

struct LogEntry {
    std::string timestamp;
    std::string message;
    bool isAction = false;
};

class AudioSessionNotificationHandler;
class AudioEndpointNotificationHandler;

struct ManagedDevice {
    std::wstring friendlyName;
    std::vector<std::wstring> targetProcesses;
    ComPtr<IMMDevice> pDevice;
    ComPtr<IAudioSessionManager2> pSessionManager;
    AudioSessionNotificationHandler* pNotificationHandler = nullptr;
    bool isConnected = false;
};

class AudioSessionMuter {
public:
    AudioSessionMuter();
    ~AudioSessionMuter();

    AudioSessionMuter(const AudioSessionMuter&) = delete;
    AudioSessionMuter& operator=(const AudioSessionMuter&) = delete;

    bool Initialize();
    void Shutdown(bool unmuteOnExit = true);

    // Master Toggle Control
    void SetEnabled(bool enable);
    bool IsEnabled() const;

    // Audio Devices Management & Enumeration
    std::vector<AudioDeviceInfo> EnumerateRenderDevices();
    void SyncManagedDevicesWithSystem();

    // Multi-Device Target Management
    void SetDeviceTargets(const std::wstring& devName, const std::vector<std::wstring>& targets);
    void AddTargetProcess(const std::wstring& devName, const std::wstring& procName);
    void RemoveTargetProcess(const std::wstring& devName, const std::wstring& procName);
    std::vector<std::wstring> GetDeviceTargets(const std::wstring& devName) const;
    void SetAllDeviceTargets(const std::map<std::wstring, std::vector<std::wstring>>& configMap);

    // Device Status Queries
    bool IsDeviceManaged(const std::wstring& devName) const;
    bool IsDeviceConnected(const std::wstring& devName) const;
    int GetManagedDeviceCount() const;
    int GetConnectedManagedDeviceCount() const;
    std::vector<std::wstring> GetManagedDeviceNames() const;

    // Active Sessions & Muting / Unmuting
    std::vector<ActiveSessionInfo> GetActiveSessions(const std::wstring& devName);
    void ScanAndMuteAllManagedDevices();
    void ScanAndMuteDevice(const std::wstring& devName);
    void UnmuteProcessForDevice(const std::wstring& devName, const std::wstring& procName);
    void UnmuteAllTargets();

    // Dynamic Device Hotplug Handler
    void OnAudioEndpointsChanged();

    // Logging
    void AddLog(const std::string& message, bool isAction = false);
    std::vector<LogEntry> GetLogs();
    uint64_t GetLogRevision() const;
    void ClearLogs();

    // Callback listeners
    void SetNotificationCallback(std::function<void(const std::wstring& devName, const std::wstring& procName, DWORD pid)> cb) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_onMutedCallback = cb;
    }

    void SetDeviceChangedCallback(std::function<void()> cb) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_onDeviceChangedCallback = cb;
    }

private:
    friend class AudioSessionNotificationHandler;

    void ProcessSessionForDevice(const std::wstring& devName, IAudioSessionControl* pSessionControl);
    bool IsMatchingProcess(DWORD pid, const std::vector<std::wstring>& targets, std::wstring& outProcName);
    std::string GetCurrentTimestamp();
    void AttachDeviceInternal(ManagedDevice& dev);
    void DetachDeviceInternal(ManagedDevice& dev);

    bool m_isEnabled = false;
    bool m_comInitialized = false;

    // Map: Device Friendly Name -> ManagedDevice
    std::map<std::wstring, ManagedDevice> m_managedDevices;

    ComPtr<IMMDeviceEnumerator> m_pEnumerator;
    AudioEndpointNotificationHandler* m_pEndpointHandler = nullptr;

    mutable std::recursive_mutex m_mutex;
    std::deque<LogEntry> m_logs;
    uint64_t m_logRevision = 0;
    std::function<void(const std::wstring&, const std::wstring&, DWORD)> m_onMutedCallback;
    std::function<void()> m_onDeviceChangedCallback;
};

class AudioSessionNotificationHandler : public IAudioSessionNotification {
    LONG m_refCount = 1;
    AudioSessionMuter* m_pParent = nullptr;
    std::wstring m_deviceName;
public:
    AudioSessionNotificationHandler(AudioSessionMuter* parent, const std::wstring& deviceName) 
        : m_pParent(parent), m_deviceName(deviceName) {}
    ~AudioSessionNotificationHandler() = default;

    AudioSessionNotificationHandler(const AudioSessionNotificationHandler&) = delete;
    AudioSessionNotificationHandler& operator=(const AudioSessionNotificationHandler&) = delete;

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ulRef = InterlockedDecrement(&m_refCount);
        if (ulRef == 0) delete this;
        return ulRef;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
            *ppv = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* NewSession) override;
};

class AudioEndpointNotificationHandler : public IMMNotificationClient {
    LONG m_refCount = 1;
    AudioSessionMuter* m_pParent = nullptr;
public:
    AudioEndpointNotificationHandler(AudioSessionMuter* parent) : m_pParent(parent) {}
    ~AudioEndpointNotificationHandler() = default;

    AudioEndpointNotificationHandler(const AudioEndpointNotificationHandler&) = delete;
    AudioEndpointNotificationHandler& operator=(const AudioEndpointNotificationHandler&) = delete;

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ulRef = InterlockedDecrement(&m_refCount);
        if (ulRef == 0) delete this;
        return ulRef;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override { return S_OK; }
};
