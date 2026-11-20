# Mask

**Mask** is a deterministic, hardware-accelerated Windows C++23 input automation, complex workflow sequencing, multi-device remapping, and screen-color trigger suite powered by **Microsoft GameInput**, **Direct2D/DirectWrite**, and **HIDMaestro v2.1.0**.

---

## Features

- **Deterministic Macro Engine (`Macro`)**
  - Click-to-bind keyboard keys and mouse buttons (`L/R/M/X1/X2`).
  - Five execution modes: **Hold-to-Loop**, **Toggle Loop**, **Repeat N Times**, **Multi-Key Sequence**, and **SOCD Pair**.
  - Sub-microsecond `__rdtscp()` + High-Resolution Waitable Timer scheduling (`50 us` floor guard) with SIMD deadline evaluation (**AVX2 + BMI2** / **SSE2**) and optional micro-jitter.
- **Screen-Color Trigger Bot (`Trigger`)**
  - Pure Ring-3 DXGI Desktop Duplication (`32x32` staging surface) with cached GDI fallback — zero memory reading and zero DLL injection.
  - Plain-English **Trigger Bot Studio** featuring a `15x15` live color loupe comparing *"Seeing now"* vs *"Target color"*, one-click **Pick color & spot** global eyedropper, **Screen center** lock, 6 quick color presets, single human-friendly **Color leeway** slider, `Color match` / `On change` modes, arming rules, and multi-monitor selection.
- **Complex Sequence Builder (`Sequence`)**
  - Two-column workflow editor supporting up to **16 sequences × 128 steps** and **8-level nested loops**.
  - Supports keyboard/mouse actions, coordinates, scrolling, delays, `Wait for color`, app launching, shell commands, layer switching, and macro toggling with timing jitter.
- **Multi-Device & Multi-Monitor Remapper (`Remap`)**
  - Up to **32 rules across 4 layers** with per-device filtering (`APP_LOCAL_DEVICE_ID`), multi-monitor cursor warping (`WarpToMonitor`), and an opt-in `SuppressionGate` that only hooks when a rule explicitly blocks/suppresses physical input.
- **Virtual Hardware Emulation & Safety**
  - Zero `SendInput`, `mouse_event`, or `keybd_event`. All synthetic output passes through `.NET 10` (`Mask.HIDMaestroBridge.dll`) into **HIDMaestro v2.1.0** UMDF2 virtual HID hardware (`337` profiles).
  - Dedicated **Watchdog** thread with `L-Ctrl + R-Ctrl + Esc` global panic chord, heartbeat stall neutralization, and automatic neutral HID release on exit or crash.
- **Sleek Zero-Idle GPU UI**
  - Native Direct2D dark-mode interface with spring animations (`Full` / `Reduced` / `Off`), 5 accent themes (`Amber`, `Cyan`, `Emerald`, `Violet`, `Crimson`), 3 atomic profile banks, Settings modal, and system tray minimization (`0%` idle CPU).

---

## Requirements

- **OS:** Windows 10 / 11 (x64)
- **Toolchain:** Visual Studio 2022 / 2026 Build Tools with MSVC (C++23, AVX2) & CMake
- **.NET SDK:** `.NET 10.0` (`net10.0-windows10.0.26100.0`) for `Mask.HIDMaestroBridge.csproj`
- **Windows SDK / GameInput:** Windows SDK + `GameInput` headers/libraries
- **Permissions:** Standard user mode launches the UI and `--self-test`; running as **Administrator** is required to claim live UMDF2 virtual HID hardware pipes via HIDMaestro.

---

## Build & Self-Test

Configure and build Release (`x64`):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --clean-first
```

Verify calibrated timer precision, SIMD fast path, GameInput device enumeration, and HIDMaestro bridge readiness:

```powershell
.\build\Release\MaskEngine.exe --self-test | Out-String
```

Launch the desktop application:

```powershell
.\build\Release\MaskEngine.exe
```

*(Tip: Run `MaskEngine.exe` as Administrator to enable live virtual mouse/keyboard output through HIDMaestro's UMDF2 driver.)*

---

## Safety Controls

- **Global Panic Chord:** `L-Ctrl + R-Ctrl + Esc` immediately pauses automation, unhooks any active suppression gate, aborts running sequences/triggers, and releases all synthetic virtual keys/buttons.
- **Pause Hotkey:** Configurable in Settings (default `F8`).
- **Protected Safety Keys:** Remap rules cannot block `Esc`, `Ctrl`, or configured Pause/Stop-All safety hotkeys.

---

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed documentation covering threading topology, lock-free queues, timing math, GameInput/HIDMaestro integration, and Direct2D UI modules.

---

## License

**Copyright (c) 2026 endrusmah. All Rights Reserved.**

This project is proprietary software owned exclusively by **endrusmah**.

- **No Selling or Commercial Use**
- **No Modification or Derivative Works**
- **No Copying or Use in Other Projects**
- **No Redistribution**

See the full terms in [LICENSE](LICENSE). *(The bundled third-party binary `lib/HIDMaestro.Core.dll` remains subject to its own third-party license terms.)*
