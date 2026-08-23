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

    // Audio Devices Management
    std::vector<AudioDeviceInfo> EnumerateRenderDevices();
    bool AttachToDevice(const std::wstring& targetNameOrSubstring, bool force = false);
    void Detach();

    // Target Filtering
    void SetTargetProcess(const std::wstring& procName);
    std::wstring GetTargetProcess() const;
    void SetTargetProcessList(const std::vector<std::wstring>& list);
    std::vector<std::wstring> GetTargetProcessList() const;
    void AddTargetProcess(const std::wstring& procName);
    void RemoveTargetProcess(const std::wstring& procName);

    bool IsAttached() const;
    std::wstring GetActiveDeviceName() const;

    // Active Sessions & Muting / Unmuting
    std::vector<ActiveSessionInfo> GetActiveSessions();
    void ScanAndMuteExistingSessions();
    void UnmuteProcess(const std::wstring& procName);
    void UnmuteAllTargets();
    void ProcessSession(IAudioSessionControl* pSessionControl);

    // Dynamic Device Hotplug Handler
    void OnAudioEndpointsChanged();

    // Logging
    void AddLog(const std::string& message, bool isAction = false);
    std::vector<LogEntry> GetLogs();
    void ClearLogs();

    // Callback listeners
    void SetNotificationCallback(std::function<void(const std::wstring& procName, DWORD pid)> cb) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_onMutedCallback = cb;
    }

    void SetDeviceChangedCallback(std::function<void()> cb) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_onDeviceChangedCallback = cb;
    }

private:
    bool IsMatchingProcess(DWORD pid, std::wstring& outProcName);
    std::string GetCurrentTimestamp();

    bool m_isEnabled = false;
    bool m_comInitialized = false;
    std::vector<std::wstring> m_targetProcesses;
    std::wstring m_activeDeviceName = L"";
    std::wstring m_lastRequestedDevice = L"Sonar - Microphone";
    bool m_isAttached = false;

    ComPtr<IMMDeviceEnumerator> m_pEnumerator;
    ComPtr<IMMDevice> m_pCurrentDevice;
    ComPtr<IAudioSessionManager2> m_pSessionManager;
    
    AudioSessionNotificationHandler* m_pNotificationHandler = nullptr;
    AudioEndpointNotificationHandler* m_pEndpointHandler = nullptr;

    mutable std::recursive_mutex m_mutex;
    std::vector<LogEntry> m_logs;
    std::function<void(const std::wstring&, DWORD)> m_onMutedCallback;
    std::function<void()> m_onDeviceChangedCallback;
};

class AudioSessionNotificationHandler : public IAudioSessionNotification {
    LONG m_refCount = 1;
    AudioSessionMuter* m_pParent = nullptr;
public:
    AudioSessionNotificationHandler(AudioSessionMuter* parent) : m_pParent(parent) {}
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
