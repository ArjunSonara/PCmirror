# PCMirror ⚡🖥️📱

> **Ultra-low latency screen mirroring, system audio streaming, and multi-touch PC control for Windows to Android.**

PCMirror provides high-performance desktop mirroring designed for gaming, productivity, and portable secondary display setups.

---

## ✨ Features

- **Ultra-Low Latency Video Streaming**:
  - Direct DXGI Desktop Duplication (GPU surface capture, event-driven).
  - Hardware H.264 encoding via Media Foundation (NVENC / QuickSync / AMD AMF) with low-latency mode and zero-lookahead.
  - Zero-copy direct TCP streaming with `TCP_NODELAY`.
  - Android hardware decoding via `MediaCodec` surface rendering.
- **PC System Sound Streaming**:
  - Real-time WASAPI Loopback capture on Windows (`AUDCLNT_STREAMFLAGS_LOOPBACK`).
  - Converts 32-bit float audio to 16-bit PCM stereo.
  - Dedicated low-latency streaming pipeline (Port 8082) decoded directly to Android `AudioTrack`.
- **Real-Time Touch Backchannel**:
  - Control your Windows desktop directly from the Android touch screen.
  - Injected via Windows `SendInput` API (Port 8081).
- **In-Stream Collapsible Gear Menu**:
  - Non-intrusive 38dp glassmorphic floating gear button.
  - Expandable controls: **Resolution**, **Bitrate / Quality**, **Aspect Ratio**, **Lock View**, **💾 Save Preset**, and **📂 Presets Browser**.
- **Lock Screen / Lock View Mode**:
  - Prevents accidental 2-finger pan/zoom gestures during intense gaming sessions or drawing.
- **Custom Aspect Ratio & Viewport Presets**:
  - Pinch-to-zoom and pan to craft custom framing or ultra-wide viewport aspect ratios.
  - Save custom presets with names (e.g. *"Gaming Ultra Fit"*, *"Full Stretch"*).
  - Quick-load or delete presets in-stream or directly from the setup dashboard.

---

## 🚀 Quick Start (Pre-built)

### 1. Connect Phone to PC
- **Option A (Fastest — ~20ms Latency)**: Enable **USB Tethering (RNDIS)** on your phone. Find the adapter IP on Windows via `ipconfig` (typically `192.168.42.x`).
- **Option B (Standard ADB Reverse)**: Enable USB Debugging on your phone and run:
  ```powershell
  .\adb.exe reverse tcp:8080 tcp:8080
  .\adb.exe reverse tcp:8081 tcp:8081
  .\adb.exe reverse tcp:8082 tcp:8082
  ```

### 2. Run Windows Server
Launch `PCMirror.exe` on your PC:
```powershell
.\PCMirror.exe 8080 1920 1080 60 40000000
```
*(Arguments: `<port> <width> <height> <fps> <bitrate>`)*

### 3. Launch Android App
Install and open `PCMirror-Android.apk` on your phone, enter your PC IP (or `127.0.0.1` for ADB reverse), and tap **START MIRROR**.

---

## 🛠️ Building from Source

### Windows Host (`mirror/windows-native/`)
- Requires **Visual Studio 2022** with C++ Desktop Development.
```powershell
cd mirror/windows-native
cmake -B build
cmake --build build --config Release
```

### Android Client (`mirror/android/`)
- Requires **Android Studio** or Android SDK (API 34+).
```powershell
cd mirror/android
.\gradlew.bat assembleDebug
```

---

## 📂 Architecture & Port Mapping

| Port | Protocol | Purpose |
| :--- | :--- | :--- |
| **8080** | TCP (Raw H.264) | DXGI Video stream |
| **8081** | TCP (Binary Packets) | Multi-touch control & mouse backchannel |
| **8082** | TCP (PCM 16-bit Stereo) | WASAPI Loopback system audio stream |

---

## 📄 License
MIT License
