# PC Mirror — USB, low-latency screen mirror (Windows → Android)

Two tiers, in increasing order of effort and decreasing order of latency.

---

## Tier 1: ffmpeg script (30-45ms) — works today

`windows/start-mirror.ps1` — same as before, but with encoder lookahead
disabled and a hardware-encode path. This still goes over adb's TCP
tunnel.

```
adb reverse tcp:8080 tcp:8080
cd windows
.\start-mirror.ps1 -Encoder nvenc     # or qsv / cpu
```

Then launch the Android app — it now asks for a server IP on launch;
for this tier, leave it as `127.0.0.1` (adb routes that to the PC).

---

## Tier 2: native app (targets 15-20ms) — the real rebuild

`windows-native/` — a compiled C++ app replacing ffmpeg entirely:
- **capture.cpp**: DXGI Desktop Duplication (event-driven GPU capture,
  not GDI polling)
- **encoder.cpp**: hardware H.264 via Media Foundation (NVENC/QuickSync/
  AMD VCE, whichever your GPU exposes), with low-latency mode forced on
  and lookahead disabled
- **network.cpp**: a raw TCP socket, `TCP_NODELAY` set, no ffmpeg pipe
  boundary in between capture/encode/send

And it drops **adb entirely** in favor of a direct USB network link:

### One-time setup
1. Install Visual Studio 2022 with the "Desktop development with C++"
   workload (gives you the Windows SDK, D3D11, and Media Foundation
   headers — no extra downloads needed).
2. Build:
   ```
   cd windows-native
   cmake -B build
   cmake --build build --config Release
   ```
3. On the phone, connect via USB, then enable **Settings → Network →
   Hotspot & tethering → USB tethering**. This creates a private
   point-to-point network over the cable itself (an RNDIS adapter on
   Windows, no internet connection needed) — this is what replaces adb
   as the transport.
4. Run `ipconfig` on the PC and find the new adapter (usually named
   "Ethernet X" or "Remote NDIS...", IP typically in the `192.168.42.x`
   range). That's the IP you'll enter in the Android app.

### Running it
1. Start the server on PC:
   ```powershell
   cd windows-native
   .\build\Release\PCMirror.exe 8080 1920 1080 60 8000000
   ```
2. Launch the Android app:
   ```powershell
   adb shell am start -n com.example.pcmirror/.MainActivity --es server_ip <PC_RNDIS_IP> --ei server_port 8080
   ```
3. Tap or drag on the Android screen to control the Windows desktop mouse in real-time (injected via `SendInput` on port 8081).

---

## Measured Performance & Benchmark Summary

Empirical testing on **Windows 11 (Intel UHD Graphics @ 144Hz)** streaming to **Nothing Phone (2a) (Android 16)**:

| Metric | Direct USB Tethering (RNDIS) | ADB Reverse |
| :--- | :--- | :--- |
| **Glass-to-Glass Latency** | **~21 – 24 ms** | ~35 – 65 ms |
| **FPS** | **50 – 52 FPS** (smooth, stable) | 5 – 28 FPS (jittery) |
| **Host Latency (Capture + Enc)**| **10.5 – 12.8 ms** (Enc: 3.9ms) | 14.5 – 43.9 ms (queue stalls) |
| **MediaCodec Decode Latency** | **14.6 ms** (with VPU clock boost) | 17.7 – 23.1 ms |
| **Transport Protocol** | Kernel RNDIS driver (`TCP_NODELAY`) | Userspace `adbd` proxy |
| **Touch Backchannel** | Supported (Port 8081) | Supported (Port 8081) |

### Honest expectations for Tier 2
- The BGRA→NV12 color conversion in encoder.cpp is a plain CPU loop,
  not SIMD-optimized. At 1080p+60fps it may itself become the
  bottleneck — profiling on your actual hardware will tell you whether
  it's worth hand-optimizing (or moving to a GPU compute shader).
- Whether you land at 16ms, 25ms, or 35ms depends on your specific GPU,
  USB controller, cable, and phone's decode speed — that number only
  comes from testing on your actual hardware, not from code review.

## Next step after this: input backchannel
Same idea as before — a second socket (or a second stream over the same
one) carrying touch coordinates from the phone, injected on Windows via
`SendInput`. Worth adding once video latency is where you want it.
