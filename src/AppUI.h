// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AudioSessionMuter.h"
#include "Config.h"
#include <string>
#include <vector>

class AppUI {
public:
    AppUI(AudioSessionMuter* muter, AppConfig* config);
    ~AppUI();

    void Initialize();
    void Render();
    void RefreshDevices();
    void ApplyModernDarkTheme();

    bool ShouldMinimizeToTray() const { return m_requestMinimizeToTray; }
    void ResetMinimizeRequest() { m_requestMinimizeToTray = false; }

private:
    AudioSessionMuter* m_pMuter = nullptr;
    AppConfig* m_pConfig = nullptr;

    std::vector<AudioDeviceInfo> m_devices;
    int m_selectedDeviceIndex = -1;
    char m_customAppInputBuffer[128] = "";

    bool m_isGuardEnabled = false;
    bool m_startWithWindows = false;
    bool m_showNotifications = true;
    bool m_startMinimized = false;
    bool m_requestMinimizeToTray = false;

    void SyncUIFromConfig();
    void SaveConfigFromUI();
    bool DrawToggleSwitch(const char* id, bool* v);
};
