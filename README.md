# Mask

**Mask** is a deterministic, hardware-accelerated Windows C++23 input automation, complex workflow sequencing, multi-device remapping, and screen-color trigger suite powered by **Microsoft GameInput**, **Direct3D 11 / Direct2D 1.1 / DirectWrite**, and **HIDMaestro v2.1.0**.

---

## Measured Performance (`--bench` & `--self-test`)

Measured on Windows x64 Release (`MaskEngine.exe --bench` and `MaskEngine.exe --self-test`, `3.2938 GHz` calibrated TSC, **AVX2 + BMI2** fast path):

- **Input-to-Output Latency** (`500` samples, physical trigger -> `EngineThreadMain` -> `outputBatchQueue` -> MMCSS `"Pro Audio"` `OutputThreadMain` -> CoreCLR `HMBridge_SubmitBatch` dry-run pipeline):
  - **`p50`**: `~4.4 us` (`4.39–4.50 us` across verified runs)
  - **`p99`**: `9.75–12.57 us`
  - **`max`**: `11.82–25.38 us`
- **Sustained Batched Output Throughput** (`10,000` clicks / `20,000` HID reports across `2,850–3,520` batches):
  - **Elapsed**: `~0.62 ms` (`0.615–0.634 ms`)
  - **Sustained Click Rate**: **`15.8M–16.3M clicks/sec`** (`15,774,140–16,270,401 clicks/sec`)
  - **HID Report Rate**: **`31.5M–32.5M HID reports/sec`** (`31,548,279–32,540,802 reports/sec`)
  - *Note:* In un-elevated runs, `--bench` uses `[CoreCLR HMBridge_SubmitBatch Dry-Run Pipeline]` to benchmark the full C++ engine, lock-free `SPSCQueue`, MMCSS `"Pro Audio"` output worker, and `.NET 10` `[UnmanagedCallersOnly]` batch marshaling path without USB/IP driver I/O. Running `MaskEngine.exe --bench` as **Administrator** switches to `[Live HIDMaestro UMDF2 Hardware]` and also exercises the physical UMDF2 virtual USB pipes (`logitech-g502-hero` and `keychron-q6`).
- **Timer Precision & Pixel Trigger ROI (`--self-test`)**:
  - **Timing error**: `p50: 9 ns` | `p99 / max: 29 ns` (`20/20 < 100 ns` on clean run; up to `9106 ns` on OS scheduler outlier runs)
  - **Trigger logic (`15x15` ROI analysis)**: `1.02–1.04 us/frame` (`PASS`)

---

## Features

- **Autoclicker Studio (`Auto` / badge `A`)** *(replaces the legacy Macro tab)*
  - Click-to-bind keyboard triggers/outputs and mouse buttons (`L/R/M/X1/X2`, plus quick mouse and scroll wheel presets).
  - Three dedicated trigger modes: **Hold** (`HoldToLoop`), **Toggle** (`InfiniteToggleLoop`), and **Burst** (`RepeatNTimes` with configurable `Repeats`).
  - Editable **CPS**, **Hold (ms)**, **Release (ms)**, **Duty cycle** slider, optional per-click pointer delta (`Move pointer` X/Y), optional timing jitter (**Exact**, **`±5%`**, **`±10%`**, **`±20%`**, **`±35%`**), and glowing **Signal view** pulse oscilloscope.
  - **Unrestricted CPS & ms values:** Allows arbitrary rates and pulse widths—including `0 ms` and `<0.05 ms`—with zero artificial floor clamping, paired with a built-in amber **stability warning banner** whenever `CPS > 1,000` or `Hold/Release < 0.05 ms` (or `0 ms`).
  - Sub-microsecond `__rdtscp()` + High-Resolution Waitable Timer scheduling with isolated **AVX2 + BMI2** deadline scanning (`src/macro_runtime_avx2.cpp`) and automatic **SSE2** fallback.
- **Screen-Color Trigger Bot (`Trigger` / `Trig`, badge `T`)**
  - Pure Ring-3 DXGI Desktop Duplication (`32x32` staging surface) with cached `32x32` GDI `DIBSection` fallback — zero process memory reading and zero DLL injection.
  - Clean **Color Match** (`MatchColor`) and **Color Change** (`ColorChanged`) modes with selectable match rules (**Any Pixel** / **Zone Average**).
  - Plain-English studio featuring a `15x15` live color loupe comparing *"Seeing now"* vs *"Target color"*, one-click **Pick color & spot** global eyedropper, **Screen center** lock, 6 quick color presets, single **Color leeway** tolerance slider, sample radius, `Always Active` / `While Key Held` / `Toggle Key` arming, `Click Key` / `Hold Key` / `Run Clicker` / `Run Sequence` actions, Delay/Hold/Cooldown (`ms`), and multi-monitor selection (up to 8 screens).
- **Complex Sequence Builder (`Sequence` / `Seq`, badge `S`)**
  - Two-column workflow studio supporting up to **16 sequences × 128 steps** and **8-level nested loops** (`LoopStart` / `LoopEnd`).
  - Supports **conditional branching (`If check` / `Else` / `End if`)** over `KeyHeld`, `ColorMatches`, `LayerIs`, `UnlockStateIs`, `MacroActive`, and `RandomChance` (with negation), alongside key/mouse actions, coordinates, scrolling, `Wait`, `Wait for color`, `Open app` (`LaunchProgram`), `Run command` (`RunCommand`), `Switch layer`, and `Toggle clicker` (`ToggleMacro`) with optional timing jitter and `RunOnce` / `ToggleLoop` / `WhileHeld` triggers.
  - **JSON Profile Import/Export** (`Export JSON` / `Import JSON` in `Settings -> Hardware & Profiles`) with an **automatic user confirmation prompt** (`MessageBoxW` trust gate and amber untrusted-import badge) before executing `LaunchProgram` or `RunCommand` steps loaded from imported JSON profiles.
- **Multi-Device & Multi-Monitor Remapping (`Remap` / `Map`, badge `R`)**
  - Up to **32 rules across 4 layers** (`Default` + 3 switchable layers) with unlock policy gating, multi-monitor cursor warping (`WarpToMonitor`), and **Raw Input hardware enumeration** (`GetRawInputDeviceList` correlated with GameInput `APP_LOCAL_DEVICE_ID`).
  - Unified hook-driven **`RemapHookPipeline`** (`WH_KEYBOARD_LL` / `WH_MOUSE_LL`) installed on demand only when active rules require interception or suppression, paired with per-VK **virtual echo tokens** so synthetic HIDMaestro outputs pass through without triggering remap loops.
- **Cosmic Blue/Black D3D11 + Direct2D 1.1 UI**
  - Native **Direct3D 11 + DXGI Flip-Model (`DXGI_SWAP_EFFECT_FLIP_DISCARD`) + Direct2D 1.1 + DirectWrite** glassmorphic UI with an embedded HLSL **Cosmic Nebula & Starfield** shader, spring animations (`Full` / `Reduced` / `Off`), 5 cosmic palettes (`Cosmic Cyan`, `Deep Sapphire`, `Ice Starlight`, `Nebula Violet`, `Aurora Teal`), 5-tab Settings overlay, 3 atomic profile banks, and system tray minimization (`--minimized`).
  - **Focused-only GPU animations** (`0%` background idle CPU when unfocused or minimized), **`TrimGpu()`** (`IDXGIDevice3::Trim()`) + working-set trim on minimize/tray hide, and multi-resolution embedded application icon (`assets/mask.ico`).
  - **Hardened & Versioned:** MSVC-only Windows x64 C++23 build, AVX2 strictly isolated to `src/macro_runtime_avx2.cpp` with automatic SSE2 fallback, `/GS` stack buffer security checks enabled, versioned crash-safe binary saves (`profiles*.bin` `v1/v2 -> v3`; `sequences*.bin`, `triggers*.bin`, `remaps*.bin`, `settings.bin` `v1 -> v2` with `.tmp`/`.bak` recovery), and UTF-8 JSON profile import/export (`profile_json` schema v2).
- **Virtual Hardware Emulation & Safety**
  - Zero `SendInput`, `mouse_event`, or `keybd_event`. All synthetic output routes through `.NET 10` (`Mask.HIDMaestroBridge.dll`) via batched `HMBridge_SubmitBatch` on an MMCSS `"Pro Audio"` output thread into **HIDMaestro v2.1.0** UMDF2 virtual HID hardware (`337` profiles).
  - Dedicated **Watchdog** thread with `L-Ctrl + R-Ctrl + Esc` global panic chord, heartbeat stall neutralization (`400 ms`), Windows session-lock stop, and automatic neutral HID release on exit or crash.

---

## Requirements

- **OS:** Windows 10 / 11 (`x64`)
- **Toolchain:** Visual Studio 2022 / 2026 Build Tools with **MSVC** (C++23) & CMake `>= 3.24` *(non-MSVC compilers and non-x64 targets are rejected by `CMakeLists.txt`)*
- **.NET SDK:** `.NET 10.0` (`net10.0-windows10.0.26100.0`) for dependency-tracked `Mask.HIDMaestroBridge.csproj` builds (`MaskBridge` target)
- **Windows SDK / GameInput:** Windows SDK (`d3d11`, `dxgi`, `d2d1`, `dwrite`, `d3dcompiler`, `comdlg32`, `avrt`, `dwmapi`, etc.) + Microsoft `GameInput` headers/libraries
- **Permissions:** Standard user mode runs the UI, `--self-test`, and dry-run `--bench`; running as **Administrator** is required to claim live UMDF2 virtual HID hardware pipes via HIDMaestro.

---

## Build, Self-Test & Benchmark

Configure and build Release (`x64`):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --clean-first
```

Verify calibrated TSC precision, SIMD equivalence, GameInput device enumeration, HIDMaestro bridge readiness, binary migration tests, and JSON round-trip self-tests:

```powershell
.\build\Release\MaskEngine.exe --self-test | Out-String
```

Run the input-to-output latency and sustained batched CPS benchmark (`--bench`):

```powershell
.\build\Release\MaskEngine.exe --bench | Out-String
```

Launch the desktop application (or start minimized to tray with `--minimized`):

```powershell
.\build\Release\MaskEngine.exe
```

*(Tip: Run `MaskEngine.exe` as Administrator to enable live virtual mouse/keyboard output through HIDMaestro's UMDF2 driver.)*

---

## Safety Controls

- **Global Panic Chord:** `L-Ctrl + R-Ctrl + Esc` immediately pauses automation, tears down active remap hooks, aborts running sequences/triggers, and releases all synthetic virtual keys/buttons.
- **Pause & Stop-All Hotkeys:** Configurable in `Settings -> Hotkeys` (default Pause: `F8`).
- **Protected Safety Keys:** Remap rules cannot block `Esc`, `Ctrl`, or configured Pause/Stop-All safety hotkeys.
- **Untrusted Imported Command Gate:** Imported JSON sequences containing `LaunchProgram` or `RunCommand` steps pause and require explicit user confirmation via prompt before executing.

---

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed documentation covering the `mask::app::App` coordinator, threading topology, lock-free queues, MMCSS batched output, isolated AVX2/SSE2 runtime, Raw Input + `RemapHookPipeline`, D3D11/D2D1.1/HLSL renderer, persistence migration, and module layout.

---

## License & Third-Party Notices

**Copyright (c) 2026 endrusmah. All Rights Reserved.**

This project is proprietary software owned exclusively by **endrusmah**.

- **No Selling or Commercial Use**
- **No Modification or Derivative Works**
- **No Copying or Use in Other Projects**
- **No Redistribution**

See the full terms in [LICENSE](LICENSE). Bundled and transitive third-party components (**HIDMaestro v2.1.0** under `MIT`, **usbip-win2 v0.9.8.1** under `BSD-2-Clause`, and **DsHidMini** under `BSD-3-Clause`) remain subject to their respective licenses; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
