// Copyright (c) 2026 Arda Ceylan. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
// SPDX-License-Identifier: Apache-2.0

#include "AppUI.h"
#include "imgui/imgui.h"
#include <algorithm>

AppUI::AppUI(AudioSessionMuter* muter, AppConfig* config)
    : m_pMuter(muter), m_pConfig(config) {
}

AppUI::~AppUI() {
}

void AppUI::Initialize() {
    ApplyModernDarkTheme();
    SyncUIFromConfig();
    RefreshDevices();
}

void AppUI::SyncUIFromConfig() {
    if (!m_pConfig || !m_pMuter) return;

    m_isGuardEnabled = m_pConfig->isEnabled;
    m_pMuter->SetEnabled(m_isGuardEnabled);
    m_pMuter->SetTargetProcess(m_pConfig->targetProcessName);

    m_startWithWindows = AppConfig::IsAutoStartEnabled();
    m_showNotifications = m_pConfig->showNotifications;
    m_startMinimized = m_pConfig->startMinimized;
}

void AppUI::SaveConfigFromUI() {
    if (!m_pConfig || !m_pMuter) return;

    m_pConfig->isEnabled = m_isGuardEnabled;
    if (m_selectedDeviceIndex >= 0 && m_selectedDeviceIndex < (int)m_devices.size()) {
        m_pConfig->targetDeviceName = m_devices[m_selectedDeviceIndex].friendlyName;
    }

    m_pConfig->targetProcessName = m_pMuter->GetTargetProcess();
    m_pConfig->startWithWindows = m_startWithWindows;
    m_pConfig->showNotifications = m_showNotifications;
    m_pConfig->startMinimized = m_startMinimized;

    m_pConfig->Save();
    AppConfig::SetAutoStart(m_startWithWindows);
    m_pMuter->SetEnabled(m_isGuardEnabled);
}

void AppUI::RefreshDevices() {
    if (!m_pMuter) return;

    m_devices = m_pMuter->EnumerateRenderDevices();
    m_selectedDeviceIndex = -1;

    std::wstring targetToFind = m_pConfig ? m_pConfig->targetDeviceName : L"Sonar - Microphone";
    std::wstring lowerTarget = Utils::ToLower(targetToFind);

    for (int i = 0; i < (int)m_devices.size(); ++i) {
        std::wstring lowerDev = Utils::ToLower(m_devices[i].friendlyName);

        if (lowerDev.find(lowerTarget) != std::wstring::npos) {
            m_selectedDeviceIndex = i;
            break;
        }
    }

    if (m_selectedDeviceIndex != -1 && m_selectedDeviceIndex < (int)m_devices.size()) {
        m_pMuter->AttachToDevice(m_devices[m_selectedDeviceIndex].friendlyName);
    } else {
        m_pMuter->Detach();
    }
}

// Pixel-perfect Fluent Animated Toggle Switch
bool AppUI::DrawToggleSwitch(const char* id, bool* v) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float width = 38.0f;
    float height = 20.0f;
    float radius = height * 0.5f;

    bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (clicked) {
        *v = !*v;
    }

    // Animation lerp
    float targetT = *v ? 1.0f : 0.0f;
    auto it = m_toggleAnimMap.find(id);
    if (it == m_toggleAnimMap.end()) {
        m_toggleAnimMap[id] = targetT;
    }
    float& animT = m_toggleAnimMap[id];
    float delta = ImGui::GetIO().DeltaTime;
    animT += (targetT - animT) * (std::min)(1.0f, delta * 16.0f);

    // Color interpolation
    bool hovered = ImGui::IsItemHovered();
    ImVec4 offCol = hovered ? ImVec4(65/255.f, 70/255.f, 88/255.f, 1.f) : ImVec4(48/255.f, 52/255.f, 66/255.f, 1.f);
    ImVec4 onCol  = hovered ? ImVec4(95/255.f, 110/255.f, 245/255.f, 1.f) : ImVec4(80/255.f, 95/255.f, 235/255.f, 1.f);

    ImVec4 currentBg = ImVec4(
        offCol.x + (onCol.x - offCol.x) * animT,
        offCol.y + (onCol.y - offCol.y) * animT,
        offCol.z + (onCol.z - offCol.z) * animT,
        1.0f
    );
    ImU32 col_bg = ImGui::ColorConvertFloat4ToU32(currentBg);

    draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col_bg, radius);
    
    float knobRadius = radius - 2.5f;
    float knobX = p.x + radius + animT * (width - radius * 2.0f);
    float knobY = p.y + radius;
    draw_list->AddCircleFilled(ImVec2(knobX, knobY), knobRadius, IM_COL32(250, 250, 255, 255));

    return clicked;
}

void AppUI::ApplyModernDarkTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    style.WindowRounding    = 10.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(14.0f, 14.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize     = 6.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.52f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.12f, 0.13f, 0.17f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.13f, 0.14f, 0.19f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.19f, 0.21f, 0.28f, 0.70f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.17f, 0.23f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.20f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.24f, 0.26f, 0.35f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.08f, 0.09f, 0.11f, 0.00f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.25f, 0.27f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.32f, 0.35f, 0.45f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.40f, 0.44f, 0.55f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.45f, 0.65f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.45f, 0.65f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);

    colors[ImGuiCol_Button]                = ImVec4(0.18f, 0.20f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.25f, 0.28f, 0.38f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.16f, 0.18f, 0.25f, 1.00f);

    colors[ImGuiCol_Header]                = ImVec4(0.22f, 0.26f, 0.40f, 0.70f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.28f, 0.33f, 0.52f, 0.85f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.34f, 0.40f, 0.65f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.18f, 0.20f, 0.26f, 0.50f);
}

void AppUI::RenderHeaderCard() {
    ImGui::BeginChild("HeaderCard", ImVec2(0, 64), true, ImGuiWindowFlags_NoScrollbar);
    
    bool isAttached = m_pMuter && m_pMuter->IsAttached();

    // Title & Version
    ImGui::SetCursorPos(ImVec2(12, 10));
    ImGui::TextColored(ImVec4(0.45f, 0.65f, 0.98f, 1.0f), "SonarCord");
    ImGui::SameLine();
    ImGui::TextDisabled("v1.1");

    // Status Line
    ImGui::SetCursorPos(ImVec2(12, 34));
    if (!m_isGuardEnabled) {
        ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.80f, 1.0f), "○  Muter Inactive (Paused)");
    } else if (isAttached) {
        ImGui::TextColored(ImVec4(0.28f, 0.88f, 0.45f, 1.0f), "●  Active (Listening)");
        ImGui::SameLine();
        std::string devName = Utils::WStringToUtf8(m_pMuter->GetActiveDeviceName());
        if (devName.length() > 24) devName = devName.substr(0, 22) + "..";
        ImGui::TextDisabled("•  %s", devName.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "●  Waiting for Device");
    }

    // Master Enable Toggle Switch
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionAvail().x - 44, 12));
    if (DrawToggleSwitch("##MasterMuterSwitch", &m_isGuardEnabled)) {
        SaveConfigFromUI();
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionAvail().x - 46, 36));
    ImGui::TextDisabled(m_isGuardEnabled ? " Active" : " Paused");

    ImGui::EndChild();
}

void AppUI::RenderDeviceCard() {
    ImGui::BeginChild("DeviceCard", ImVec2(0, 68), true, ImGuiWindowFlags_NoScrollbar);
    
    ImGui::SetCursorPos(ImVec2(12, 8));
    ImGui::TextDisabled("Audio Output Device");

    std::string currentItemPreview = (m_selectedDeviceIndex >= 0 && m_selectedDeviceIndex < (int)m_devices.size()) 
        ? Utils::WStringToUtf8(m_devices[m_selectedDeviceIndex].friendlyName) 
        : "Select audio device...";

    ImGui::SetCursorPos(ImVec2(12, 30));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
    if (ImGui::BeginCombo("##AudioDevicesCombo", currentItemPreview.c_str())) {
        for (int i = 0; i < (int)m_devices.size(); i++) {
            bool isSelected = (m_selectedDeviceIndex == i);
            std::string devName = Utils::WStringToUtf8(m_devices[i].friendlyName);

            if (ImGui::Selectable(devName.c_str(), isSelected)) {
                m_selectedDeviceIndex = i;
                m_pMuter->AttachToDevice(m_devices[i].friendlyName);
                SaveConfigFromUI();
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetCursorPosY(30);
    if (ImGui::Button("Refresh", ImVec2(70, 0))) {
        RefreshDevices();
        SaveConfigFromUI();
    }

    ImGui::EndChild();
}

void AppUI::RenderAppsCard() {
    ImGui::BeginChild("AppsCard", ImVec2(0, 150), true);
    
    ImGui::SetCursorPos(ImVec2(12, 8));
    ImGui::TextDisabled("Applications (Click to toggle mute)");

    // Collect unified application list: Active sessions + configured targets
    struct DisplayApp {
        std::wstring name;
        bool isTarget;
        bool isActive;
        DWORD pid;
    };
    std::vector<DisplayApp> displayList;

    m_sessionUpdateTimer += ImGui::GetIO().DeltaTime;
    if (m_sessionUpdateTimer >= 1.0f || m_cachedSessions.empty()) {
        if (m_pMuter) m_cachedSessions = m_pMuter->GetActiveSessions();
        m_sessionUpdateTimer = 0.0f;
    }

    if (m_pMuter) {
        const auto& activeSessions = m_cachedSessions;
        auto targetList = m_pMuter->GetTargetProcessList();

        // 1. Add active sessions
        for (const auto& sess : activeSessions) {
            if (sess.processName == L"System/Unknown") continue;
            displayList.push_back({ sess.processName, sess.isTarget, true, sess.pid });
        }

        // 2. Add configured targets that might not be currently active
        for (const auto& target : targetList) {
            bool alreadyInList = false;
            for (const auto& d : displayList) {
                if (Utils::ProcessNamesMatch(d.name, target)) {
                    alreadyInList = true;
                    break;
                }
            }
            if (!alreadyInList) {
                displayList.push_back({ target, true, false, 0 });
            }
        }
    }

    // Clickable interactive tiles list
    ImGui::SetCursorPos(ImVec2(10, 28));
    ImGui::BeginChild("AppsList", ImVec2(0, 76), false);
    if (displayList.empty()) {
        ImGui::SetCursorPosY(22);
        ImGui::TextDisabled("   No active audio streams. Add applications below.");
    } else {
        for (size_t i = 0; i < displayList.size(); ++i) {
            const auto& app = displayList[i];
            std::string pName = Utils::WStringToUtf8(app.name);

            ImGui::PushID((int)i);
            ImVec2 itemPos = ImGui::GetCursorScreenPos();
            float itemWidth = ImGui::GetContentRegionAvail().x - 4.0f;
            float itemHeight = 32.0f;

            if (ImGui::InvisibleButton("##AppRow", ImVec2(itemWidth, itemHeight))) {
                if (app.isTarget) {
                    m_pMuter->RemoveTargetProcess(app.name);
                } else {
                    m_pMuter->AddTargetProcess(app.name);
                }
                SaveConfigFromUI();
            }

            bool isHovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // Dynamic background: Red tinted when armed/muted, subtle slate when normal
            ImU32 rowBg, borderCol;
            if (app.isTarget) {
                rowBg = isHovered ? IM_COL32(56, 22, 28, 255) : IM_COL32(40, 18, 24, 255);
                borderCol = IM_COL32(190, 50, 65, 180);
            } else {
                rowBg = isHovered ? IM_COL32(28, 30, 42, 255) : IM_COL32(20, 22, 30, 255);
                borderCol = IM_COL32(36, 40, 54, 120);
            }

            drawList->AddRectFilled(itemPos, ImVec2(itemPos.x + itemWidth, itemPos.y + itemHeight), rowBg, 6.0f);
            drawList->AddRect(itemPos, ImVec2(itemPos.x + itemWidth, itemPos.y + itemHeight), borderCol, 6.0f);

            // Precise optical vertical alignment
            float textY = itemPos.y + (itemHeight - ImGui::GetFontSize()) * 0.5f;
            float circleCenterY = textY + ImGui::GetFontSize() * 0.48f;
            float circleX = itemPos.x + 14.0f;

            // Circle Indicator (Red filled when targeted, hollow when normal)
            if (app.isTarget) {
                drawList->AddCircleFilled(ImVec2(circleX, circleCenterY), 4.5f, IM_COL32(235, 60, 75, 255));
            } else {
                drawList->AddCircle(ImVec2(circleX, circleCenterY), 4.0f, IM_COL32(130, 140, 160, 255), 16, 1.2f);
            }

            // Process Name Text
            ImVec2 textPos = ImVec2(itemPos.x + 28.0f, textY);
            ImU32 textCol = app.isTarget ? IM_COL32(250, 250, 255, 255) : IM_COL32(195, 200, 215, 255);
            drawList->AddText(textPos, textCol, pName.c_str());

            // Right Side Status Tag ("Mute" instead of "MUTED")
            if (app.isTarget) {
                const char* tag = "Mute";
                ImVec2 tagSize = ImGui::CalcTextSize(tag);
                ImVec2 tagPos = ImVec2(itemPos.x + itemWidth - tagSize.x - 14.0f, textY);
                drawList->AddText(tagPos, IM_COL32(235, 75, 90, 255), tag);
            } else if (isHovered) {
                const char* tag = "+ Click to mute";
                ImVec2 tagSize = ImGui::CalcTextSize(tag);
                ImVec2 tagPos = ImVec2(itemPos.x + itemWidth - tagSize.x - 14.0f, textY);
                drawList->AddText(tagPos, IM_COL32(135, 145, 175, 255), tag);
            }

            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();

    // Bottom Manual Input Line
    ImGui::SetCursorPos(ImVec2(12, 112));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 72);
    ImGui::InputTextWithHint("##CustomApp", "Enter process name (e.g. Discord.exe)", m_customAppInputBuffer, sizeof(m_customAppInputBuffer));
    
    ImGui::SameLine();
    if (ImGui::Button("+ Add", ImVec2(64, 0))) {
        std::string appStr = m_customAppInputBuffer;
        if (!appStr.empty()) {
            if (m_pMuter) {
                m_pMuter->AddTargetProcess(Utils::Utf8ToWString(appStr));
                SaveConfigFromUI();
            }
            m_customAppInputBuffer[0] = '\0';
        }
    }

    ImGui::EndChild();
}

void AppUI::RenderSettingsCard() {
    ImGui::BeginChild("SettingsCard", ImVec2(0, 44), true, ImGuiWindowFlags_NoScrollbar);
    
    float availWidth = ImGui::GetContentRegionAvail().x;
    float colWidth = (availWidth - 10.0f) * 0.5f;

    // Option 1: Start with Windows
    ImGui::SetCursorPos(ImVec2(12, 12));
    if (DrawToggleSwitch("##StartWithWindowsSwitch", &m_startWithWindows)) {
        SaveConfigFromUI();
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY(12);
    ImGui::Text("Start with Windows");

    // Option 2: Show Notifications
    ImGui::SameLine(colWidth + 16.0f);
    ImGui::SetCursorPosY(12);
    if (DrawToggleSwitch("##ShowNotificationsSwitch", &m_showNotifications)) {
        SaveConfigFromUI();
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY(12);
    ImGui::Text("Notifications");

    ImGui::EndChild();
}

void AppUI::RenderActivityCard() {
    ImGui::BeginChild("ActivityCard", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
    
    // Header Row: Title & Right-Aligned Clear Button
    ImGui::SetCursorPos(ImVec2(12, 10));
    ImGui::TextDisabled("Recent Activity");

    float clearBtnWidth = 48.0f;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - clearBtnWidth - 12.0f, 6.0f));
    if (ImGui::Button("Clear", ImVec2(clearBtnWidth, 0))) {
        if (m_pMuter) m_pMuter->ClearLogs();
    }

    // Log Content Box (Fills entire rest of card)
    ImGui::SetCursorPos(ImVec2(8, 36));
    ImGui::BeginChild("LogContent", ImVec2(0, ImGui::GetContentRegionAvail().y - 8.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
    
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
    if (m_pMuter) {
        auto logs = m_pMuter->GetLogs();
        if (logs.empty()) {
            ImGui::SetCursorPosY(40);
            ImGui::TextDisabled("   No audio events yet.");
        } else {
            for (const auto& log : logs) {
                ImGui::TextDisabled("[%s]", log.timestamp.c_str());
                ImGui::SameLine();
                if (log.isAction) {
                    ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.55f, 1.0f), "●  %s", log.message.c_str());
                } else {
                    ImGui::TextUnformatted(log.message.c_str());
                }
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
    }
    ImGui::PopStyleVar();

    ImGui::EndChild();

    ImGui::EndChild();
}

void AppUI::Render() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | 
                                   ImGuiWindowFlags_NoMove | 
                                   ImGuiWindowFlags_NoResize | 
                                   ImGuiWindowFlags_NoSavedSettings | 
                                   ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("MainPanel", nullptr, windowFlags);

    // 1. Header & Master Switch Card
    RenderHeaderCard();
    ImGui::Spacing();

    // 2. Audio Output Device Card
    RenderDeviceCard();
    ImGui::Spacing();

    // 3. Applications List Card
    RenderAppsCard();
    ImGui::Spacing();

    // 4. Settings Card
    RenderSettingsCard();
    ImGui::Spacing();

    // 5. Recent Activity Card
    RenderActivityCard();

    ImGui::End();
}
