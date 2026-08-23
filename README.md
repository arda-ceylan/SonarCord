# SonarCord 🎧⚡

<div align="center">

![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011%20x64-blue.svg)
![C++](https://img.shields.io/badge/language-C%2B%2B17-00599C.svg)
![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)

**Ultra-lightweight, hardware-efficient real-time audio session muter for SteelSeries Sonar and Discord.**

[Features](#-key-features) • [The Problem](#-the-problem) • [How It Works](#-how-it-works) • [Installation](#-installation) • [Building from Source](#-building-from-source) • [License](#-license)

</div>

---

## 🎯 The Problem

When streaming games or sharing your screen on **Discord** while using **SteelSeries Sonar**, Discord captures audio output directly from the *"SteelSeries Sonar - Microphone"* render endpoint. 

Because Discord often automatically opens an active audio output session under this microphone channel, your own voice, your friends' voices or game sound effects get fed straight back into your stream — creating an annoying, persistent **mic echo / audio feedback loop**.

---

## 💡 The Solution

**SonarCord** runs silently in the Windows system tray and utilizes the native **Windows Core Audio (WASAPI)** event bus. 

As soon as Discord (or any user-defined application) spawns an audio playback session under the SteelSeries Sonar microphone device, SonarCord intercepts the event within milliseconds and **automatically mutes the session** — eliminating microphone echo forever with **zero manual effort**.

---

## ✨ Key Features

* 🚀 **Zero CPU & Ultra-Low Memory:** Consumes **0.0% CPU** and **~15 MB RAM** when running in the background.
* ⚡ **Event-Driven WASAPI Architecture:** No polling loops or CPU spinlocks; wakes up only when an audio session is created or modified.
* 🔊 **Seamless Auto-Unmute:** Automatically restores audio volume when an application is untoggled or when the master muter is paused.
* 🎨 **Fluent Modern Dark UI:** Hardware-accelerated DirectX 11 + Dear ImGui interface.
* 📥 **System Tray Native:** Seamlessly hides to the tray, Windows toast notifications and auto-start on Windows boot.
* 🎛️ **Multi-App Target Filtering:** Intercept multiple applications simultaneously (Discord, Spotify, games, etc.) with clickable interactive tiles or manual process entry.

---

## 🏗️ How It Works (Architecture)

```
                       Windows WASAPI Audio Pipeline
                                     │
                 [ IAudioSessionNotification Callback ]
                                     │
                                     ▼
                     ┌───────────────────────────────┐
                     │    SonarCord Core Engine      │
                     │  (MTA Thread / Event-Driven)  │
                     └───────────────┬───────────────┘
                                     │
            Matches Target Process?  ├───────────────┐
                     │ (YES)                         │ (NO)
                     ▼                               ▼
       ┌───────────────────────────┐         [ Ignore Session ]
       │ ISimpleAudioVolume::      │
       │ SetMute(TRUE, NULL)       │
       └─────────────┬─────────────┘
                     │
                     ▼
          [ Echo Eliminated! 🔇 ]
```

1. Registers an `IAudioSessionNotification` handler with the target audio endpoint.
2. COM callbacks invoke `OnSessionCreated` instantly upon any new audio stream.
3. Automatically evaluates the caller's process ID (PID) and executable name via `QueryFullProcessImageNameW`.
4. If matched, issues an atomic `ISimpleAudioVolume::SetMute(TRUE)` call.

---

## 📥 Installation

1. Download the latest `SonarCord.exe` from the [Releases](https://github.com/arda-ceylan/SonarCord/releases) section.
2. Run `SonarCord.exe`.
3. Select your audio device (e.g., *SteelSeries Sonar - Microphone*), toggle **Active**, and click to arm the applications you want to mute.
4. Minimize to tray and enjoy crystal-clear streams without echoes!

---

## 🛠️ Building from Source

### Prerequisites
* Windows 10 or Windows 11 (64-bit)
* Visual Studio 2022 / 2026 with **Desktop development with C++** workload
* Windows 10/11 SDK

### Build via Visual Studio
1. Clone the repository:
   ```bash
   git clone https://github.com/arda-ceylan/SonarCord.git
   cd SonarCord
   ```
2. Open `SonarCord.sln` in Visual Studio.
3. Select **Release** and **x64**.
4. Press `Ctrl + Shift + B` (Build Solution).
5. The standalone executable will be located in `bin/Release/SonarCord.exe`.

### Build via Command Line (MSBuild)
```powershell
MSBuild.exe SonarCord.sln /p:Configuration=Release /p:Platform=x64 /m
```

---

## 📄 License

This project is licensed under the **Apache License 2.0**. See the [LICENSE](LICENSE) file for details.
