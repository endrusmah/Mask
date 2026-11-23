using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;
using HIDMaestro;

namespace Mask.Bridge
{
    /// <summary>
    /// Native-Callable Unmanaged Entry Points bridging C++23 MaskEngine directly to HIDMaestro v2.1.0.
    /// Zero runtime heap allocations on the real-time SubmitMouse / SubmitKeyboard hot paths.
    /// </summary>
    public static unsafe class NativeExports
    {
        private static readonly object s_lifecycleLock = new object();

        // Lightweight atomic spin guards for real-time mouse/keyboard hot paths (avoids Monitor contention)
        private static int s_mouseGuard;    // 0 = unlocked, 1 = locked
        private static int s_keyboardGuard; // 0 = unlocked, 1 = locked

        private static HMContext? s_context;
        private static HMController? s_mouseController;
        private static HMController? s_keyboardController;

        // Pre-allocated 6-key rollover buffer for zero-allocation Span slicing on the hot path
        private static readonly HMKey[] s_keyRolloverBuffer = new HMKey[6];

        private static volatile bool s_initialized;
        private static volatile bool s_driverInstalledCached;
        private static volatile bool s_mouseFaulted;
        private static volatile bool s_keyboardFaulted;
        private static volatile int s_mouseTransportIndex = -1;
        private static volatile int s_keyboardTransportIndex = -1;
        private static volatile int s_loadedProfileCount;
        private static volatile int s_activeMouseVid;
        private static volatile int s_activeMousePid;
        private static volatile int s_activeKeyboardVid;
        private static volatile int s_activeKeyboardPid;
        private static string s_lastStatus = "Uninitialized";
        private static string s_activeMouseProfileId = "logitech-g502-hero";
        private static string s_activeKeyboardProfileId = "keychron-q6";
        private static string? s_liveMouseProfileId;
        private static string? s_liveKeyboardProfileId;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static void AcquireSpinGuard(ref int guard)
        {
            if (Interlocked.CompareExchange(ref guard, 1, 0) == 0)
            {
                return;
            }
            AcquireSpinGuardSlow(ref guard);
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void AcquireSpinGuardSlow(ref int guard)
        {
            SpinWait spinner = default;
            while (Interlocked.CompareExchange(ref guard, 1, 0) != 0)
            {
                spinner.SpinOnce();
            }
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static bool TryAcquireSpinGuardBounded(ref int guard, int maxSpins)
        {
            for (int i = 0; i < maxSpins; ++i)
            {
                if (Interlocked.CompareExchange(ref guard, 1, 0) == 0)
                {
                    return true;
                }
                Thread.SpinWait(1);
            }
            return false;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static void ReleaseSpinGuard(ref int guard)
        {
            Volatile.Write(ref guard, 0);
        }

        private static bool IsProcessElevated()
        {
            using WindowsIdentity identity = WindowsIdentity.GetCurrent();
            WindowsPrincipal principal = new WindowsPrincipal(identity);
            return principal.IsInRole(WindowsBuiltInRole.Administrator);
        }

        private static void WriteWideString(string src, char* dst, int maxChars)
        {
            if (dst == null || maxChars <= 0)
            {
                return;
            }
            if (string.IsNullOrEmpty(src))
            {
                dst[0] = '\0';
                return;
            }
            int copyLen = Math.Min(src.Length, maxChars - 1);
            src.AsSpan(0, copyLen).CopyTo(new Span<char>(dst, maxChars));
            dst[copyLen] = '\0';
        }

        /// <summary>
        /// Initializes HIDMaestro v2.1.0 context, loads all 337 hardware profiles, verifies/installs the UMDF2 driver,
        /// and creates the virtual hardware mouse and keyboard controllers.
        /// Return codes:
        ///   0 = Full Hardware Virtualization Active (Controllers Live)
        ///   1 = Profiles Loaded & Driver Installed, but process requires Administrator elevation to claim ProtectedPrefix pipes
        ///  <0 = Initialization Failure
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
        public static int HMBridge_Initialize(
            char* mouseProfileIdPtr,
            char* keyboardProfileIdPtr,
            char* statusOutBuffer,
            int statusOutBufferChars)
        {
            lock (s_lifecycleLock)
            {
                try
                {
                    string requestedMouseId = (mouseProfileIdPtr != null && *mouseProfileIdPtr != '\0')
                        ? new string(mouseProfileIdPtr)
                        : "logitech-g502-hero";

                    string requestedKeyboardId = (keyboardProfileIdPtr != null && *keyboardProfileIdPtr != '\0')
                        ? new string(keyboardProfileIdPtr)
                        : "keychron-q6";

                    s_activeMouseProfileId = requestedMouseId;
                    s_activeKeyboardProfileId = requestedKeyboardId;

                    if (s_context == null)
                    {
                        s_context = new HMContext();
                        s_loadedProfileCount = s_context.LoadDefaultProfiles();
                    }

                    bool elevated = IsProcessElevated();
                    bool driverAlreadyInstalled = s_context.IsDriverInstalled;
                    s_driverInstalledCached = driverAlreadyInstalled;

                    if (!elevated)
                    {
                        s_initialized = false;
                        s_activeMouseVid = 0;
                        s_activeMousePid = 0;
                        s_activeKeyboardVid = 0;
                        s_activeKeyboardPid = 0;
                        s_lastStatus = driverAlreadyInstalled
                            ? $"HIDMaestro v2.1.0 Ready ({s_loadedProfileCount} profiles, UMDF2 Installed). Run as Administrator to claim virtual hardware pipes."
                            : $"HIDMaestro v2.1.0 Loaded ({s_loadedProfileCount} profiles). Run as Administrator to deploy UMDF2 driver & claim devices.";
                        WriteWideString(s_lastStatus, statusOutBuffer, statusOutBufferChars);
                        return 1;
                    }

                    // Only run driver install check when the UMDF2 driver is not already installed.
                    if (!driverAlreadyInstalled)
                    {
                        s_context.InstallDriver();
                        s_driverInstalledCached = s_context.IsDriverInstalled;
                    }

                    HMProfile? mouseProfile = s_context.GetProfile(requestedMouseId)
                                              ?? s_context.GetProfile("logitech-g502-hero");
                    HMProfile? kbProfile = s_context.GetProfile(requestedKeyboardId)
                                           ?? s_context.GetProfile("keychron-q6");

                    if (mouseProfile == null || kbProfile == null)
                    {
                        s_initialized = false;
                        s_lastStatus = "HIDMaestro Error: Required keyboard/mouse profile missing from catalog.";
                        WriteWideString(s_lastStatus, statusOutBuffer, statusOutBufferChars);
                        return -2;
                    }

                    HMController? curMouse = Volatile.Read(ref s_mouseController);
                    HMController? curKb = Volatile.Read(ref s_keyboardController);

                    // Sweep orphaned virtual devices ONLY on cold startup (never during live device swaps).
                    if (curMouse == null && curKb == null)
                    {
                        HMContext.RemoveAllVirtualControllers(HMCleanupScope.Orphaned, preserveInstall: true);
                    }

                    bool needMouse = (curMouse == null) ||
                                     s_mouseFaulted ||
                                     !string.Equals(s_liveMouseProfileId, mouseProfile.Id, StringComparison.OrdinalIgnoreCase);
                    bool needKb = (curKb == null) ||
                                  s_keyboardFaulted ||
                                  !string.Equals(s_liveKeyboardProfileId, kbProfile.Id, StringComparison.OrdinalIgnoreCase);

                    if (!needMouse && !needKb)
                    {
                        s_activeMouseVid = mouseProfile.VendorId;
                        s_activeMousePid = mouseProfile.ProductId;
                        s_activeKeyboardVid = kbProfile.VendorId;
                        s_activeKeyboardPid = kbProfile.ProductId;
                        s_initialized = true;
                        Version? liveVer = curMouse!.DriverVersion ?? curKb!.DriverVersion;
                        s_lastStatus = $"HIDMaestro v2.1.0 LIVE [Drv {liveVer?.ToString() ?? "2.1.0"}] | Mouse: {mouseProfile.Name} (#{s_mouseTransportIndex}) | KB: {kbProfile.Name} (#{s_keyboardTransportIndex})";
                        WriteWideString(s_lastStatus, statusOutBuffer, statusOutBufferChars);
                        return 0;
                    }

                    if (needMouse && needKb)
                    {
                        s_initialized = false;
                    }

                    // 1. Briefly acquire ONLY the guard(s) for controller(s) being swapped to detach old references.
                    HMController? oldMouse = null;
                    HMController? oldKb = null;

                    if (needMouse)
                    {
                        bool mouseLocked = false;
                        try
                        {
                            AcquireSpinGuard(ref s_mouseGuard);
                            mouseLocked = true;
                            oldMouse = Interlocked.Exchange(ref s_mouseController, null);
                        }
                        finally
                        {
                            if (mouseLocked)
                            {
                                ReleaseSpinGuard(ref s_mouseGuard);
                            }
                        }
                        s_liveMouseProfileId = null;
                        s_mouseTransportIndex = -1;
                        s_activeMouseVid = 0;
                        s_activeMousePid = 0;
                        s_mouseFaulted = false;
                    }

                    if (needKb)
                    {
                        bool kbLocked = false;
                        try
                        {
                            AcquireSpinGuard(ref s_keyboardGuard);
                            kbLocked = true;
                            oldKb = Interlocked.Exchange(ref s_keyboardController, null);
                        }
                        finally
                        {
                            if (kbLocked)
                            {
                                ReleaseSpinGuard(ref s_keyboardGuard);
                            }
                        }
                        s_liveKeyboardProfileId = null;
                        s_keyboardTransportIndex = -1;
                        s_activeKeyboardVid = 0;
                        s_activeKeyboardPid = 0;
                        s_keyboardFaulted = false;
                    }

                    // 2. Neutral-release and dispose old controller(s) OUTSIDE all spinlocks.
                    if (oldMouse != null)
                    {
                        try
                        {
                            HMMouseState neutralOldMouse = default;
                            oldMouse.SubmitMouse(in neutralOldMouse);
                        }
                        catch
                        {
                            // Best-effort neutral release before dispose
                        }
                    }
                    if (oldKb != null)
                    {
                        try
                        {
                            oldKb.SubmitKeyboard(HMKeyModifiers.None, ReadOnlySpan<HMKey>.Empty);
                        }
                        catch
                        {
                            // Best-effort neutral release before dispose
                        }
                    }
                    if (oldMouse != null && oldKb != null)
                    {
                        s_context.DisposeControllersInParallel(new HMController[] { oldMouse, oldKb }, null);
                    }
                    else
                    {
                        oldMouse?.Dispose();
                        oldKb?.Dispose();
                    }

                    // 3. Create + initialize neutral state on new controller(s) OUTSIDE all spinlocks.
                    //    On cold startup (or when both change), create Mouse & Keyboard in parallel to halve PnP latency.
                    HMController? newMouse = null;
                    HMController? newKb = null;
                    HMContext ctx = s_context;

                    if (needMouse && needKb)
                    {
                        Task<HMController> mouseTask = Task.Run(() =>
                        {
                            HMController c = ctx.CreateController(mouseProfile, "mask-hw-mouse-0");
                            HMMouseState neutralMouse = default;
                            c.SubmitMouse(in neutralMouse);
                            return c;
                        });
                        Task<HMController> kbTask = Task.Run(() =>
                        {
                            HMController c = ctx.CreateController(kbProfile, "mask-hw-keyboard-0");
                            c.SubmitKeyboard(HMKeyModifiers.None, ReadOnlySpan<HMKey>.Empty);
                            return c;
                        });

                        try
                        {
                            Task.WaitAll(mouseTask, kbTask);
                            newMouse = mouseTask.Result;
                            newKb = kbTask.Result;
                        }
                        catch
                        {
                            if (mouseTask.IsCompletedSuccessfully)
                            {
                                try { mouseTask.Result.Dispose(); } catch { }
                            }
                            if (kbTask.IsCompletedSuccessfully)
                            {
                                try { kbTask.Result.Dispose(); } catch { }
                            }
                            throw;
                        }
                    }
                    else if (needMouse)
                    {
                        newMouse = ctx.CreateController(mouseProfile, "mask-hw-mouse-0");
                        HMMouseState neutralMouse = default;
                        newMouse.SubmitMouse(in neutralMouse);
                    }
                    else if (needKb)
                    {
                        newKb = ctx.CreateController(kbProfile, "mask-hw-keyboard-0");
                        newKb.SubmitKeyboard(HMKeyModifiers.None, ReadOnlySpan<HMKey>.Empty);
                    }

                    // 4. Briefly acquire guard ONLY to store the live controller reference + metadata, then release immediately.
                    if (needMouse && newMouse != null)
                    {
                        bool mouseLocked = false;
                        try
                        {
                            AcquireSpinGuard(ref s_mouseGuard);
                            mouseLocked = true;
                            Volatile.Write(ref s_mouseController, newMouse);
                            s_mouseTransportIndex = newMouse.TransportIndex;
                            s_activeMouseVid = mouseProfile.VendorId;
                            s_activeMousePid = mouseProfile.ProductId;
                            s_liveMouseProfileId = mouseProfile.Id;
                            s_mouseFaulted = false;
                        }
                        finally
                        {
                            if (mouseLocked)
                            {
                                ReleaseSpinGuard(ref s_mouseGuard);
                            }
                        }
                    }

                    if (needKb && newKb != null)
                    {
                        bool kbLocked = false;
                        try
                        {
                            AcquireSpinGuard(ref s_keyboardGuard);
                            kbLocked = true;
                            Volatile.Write(ref s_keyboardController, newKb);
                            s_keyboardTransportIndex = newKb.TransportIndex;
                            s_activeKeyboardVid = kbProfile.VendorId;
                            s_activeKeyboardPid = kbProfile.ProductId;
                            s_liveKeyboardProfileId = kbProfile.Id;
                            s_keyboardFaulted = false;
                        }
                        finally
                        {
                            if (kbLocked)
                            {
                                ReleaseSpinGuard(ref s_keyboardGuard);
                            }
                        }
                    }

                    HMController? liveMouse = Volatile.Read(ref s_mouseController);
                    HMController? liveKb = Volatile.Read(ref s_keyboardController);
                    s_initialized = (liveMouse != null && liveKb != null);
                    Version? drvVer = liveMouse?.DriverVersion ?? liveKb?.DriverVersion;
                    s_lastStatus = $"HIDMaestro v2.1.0 LIVE [Drv {drvVer?.ToString() ?? "2.1.0"}] | Mouse: {mouseProfile.Name} (#{s_mouseTransportIndex}) | KB: {kbProfile.Name} (#{s_keyboardTransportIndex})";
                    WriteWideString(s_lastStatus, statusOutBuffer, statusOutBufferChars);
                    return 0;
                }
                catch (Exception ex)
                {
                    Exception actual = (ex is AggregateException agg && agg.InnerException != null)
                        ? agg.InnerException
                        : ex;
                    s_initialized = (Volatile.Read(ref s_mouseController) != null && Volatile.Read(ref s_keyboardController) != null);
                    if (actual is UnauthorizedAccessException)
                    {
                        s_lastStatus = $"HIDMaestro Elevation Required: {actual.Message}";
                        WriteWideString(s_lastStatus, statusOutBuffer, statusOutBufferChars);
                        return 1;
                    }
                    s_lastStatus = $"HIDMaestro Init Exception ({actual.GetType().Name}): {actual.Message}";
                    WriteWideString(s_lastStatus, statusOutBuffer, statusOutBufferChars);
                    return -1;
                }
            }
        }

        /// <summary>
        /// Submits a discrete hardware mouse state (buttons + relative X/Y/Wheel/Pan movement) to the virtual mouse.
        /// Zero heap allocations on the hot path.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
        [MethodImpl(MethodImplOptions.AggressiveOptimization)]
        public static int HMBridge_SubmitMouse(uint buttonsMask, int dx, int dy, int wheel, int pan)
        {
            if (!s_initialized)
            {
                return -1;
            }

            bool locked = false;
            try
            {
                AcquireSpinGuard(ref s_mouseGuard);
                locked = true;

                HMController? mouse = Volatile.Read(ref s_mouseController);
                if (!s_initialized || mouse == null)
                {
                    return -1;
                }

                HMMouseState state = new HMMouseState
                {
                    Buttons = (HMMouseButtons)buttonsMask,
                    X = dx,
                    Y = dy,
                    Wheel = wheel,
                    Pan = pan
                };
                mouse.SubmitMouse(in state);
                return 0;
            }
            catch
            {
                s_mouseFaulted = true;
                return -2;
            }
            finally
            {
                if (locked)
                {
                    ReleaseSpinGuard(ref s_mouseGuard);
                }
            }
        }

        /// <summary>
        /// Submits a complete 6-key rollover + modifier bitmask keyboard report to the virtual keyboard.
        /// Zero heap allocations on the hot path.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
        [MethodImpl(MethodImplOptions.AggressiveOptimization)]
        public static int HMBridge_SubmitKeyboard(byte modifiersMask, byte* hidKeyCodes, int keyCount)
        {
            if (!s_initialized)
            {
                return -1;
            }

            bool locked = false;
            try
            {
                AcquireSpinGuard(ref s_keyboardGuard);
                locked = true;

                HMController? kb = Volatile.Read(ref s_keyboardController);
                if (!s_initialized || kb == null)
                {
                    return -1;
                }

                int clampedCount = Math.Clamp(keyCount, 0, 6);
                if (clampedCount == 0 || hidKeyCodes == null)
                {
                    kb.SubmitKeyboard((HMKeyModifiers)modifiersMask, ReadOnlySpan<HMKey>.Empty);
                }
                else
                {
                    for (int i = 0; i < clampedCount; ++i)
                    {
                        s_keyRolloverBuffer[i] = (HMKey)hidKeyCodes[i];
                    }
                    ReadOnlySpan<HMKey> span = new ReadOnlySpan<HMKey>(s_keyRolloverBuffer, 0, clampedCount);
                    kb.SubmitKeyboard((HMKeyModifiers)modifiersMask, span);
                }
                return 0;
            }
            catch
            {
                s_keyboardFaulted = true;
                return -2;
            }
            finally
            {
                if (locked)
                {
                    ReleaseSpinGuard(ref s_keyboardGuard);
                }
            }
        }

        private static int ReleaseAllNeutralInternal(bool bestEffortBounded)
        {
            int result = 0;
            if (Volatile.Read(ref s_mouseController) != null)
            {
                bool locked = false;
                try
                {
                    if (bestEffortBounded)
                    {
                        locked = TryAcquireSpinGuardBounded(ref s_mouseGuard, 20000);
                    }
                    else
                    {
                        AcquireSpinGuard(ref s_mouseGuard);
                        locked = true;
                    }

                    if (locked)
                    {
                        HMController? mouse = Volatile.Read(ref s_mouseController);
                        if (mouse != null)
                        {
                            HMMouseState emptyMouse = default;
                            mouse.SubmitMouse(in emptyMouse);
                        }
                    }
                }
                catch
                {
                    result = -1;
                }
                finally
                {
                    if (locked)
                    {
                        ReleaseSpinGuard(ref s_mouseGuard);
                    }
                }
            }

            if (Volatile.Read(ref s_keyboardController) != null)
            {
                bool locked = false;
                try
                {
                    if (bestEffortBounded)
                    {
                        locked = TryAcquireSpinGuardBounded(ref s_keyboardGuard, 20000);
                    }
                    else
                    {
                        AcquireSpinGuard(ref s_keyboardGuard);
                        locked = true;
                    }

                    if (locked)
                    {
                        HMController? kb = Volatile.Read(ref s_keyboardController);
                        if (kb != null)
                        {
                            kb.SubmitKeyboard(HMKeyModifiers.None, ReadOnlySpan<HMKey>.Empty);
                        }
                    }
                }
                catch
                {
                    result = -2;
                }
                finally
                {
                    if (locked)
                    {
                        ReleaseSpinGuard(ref s_keyboardGuard);
                    }
                }
            }

            return result;
        }

        /// <summary>
        /// Driver-Level Fail-Safe Neutralizer:
        /// Immediately transmits zeroed neutral reports to both the virtual mouse and virtual keyboard
        /// so no button or key can remain held in the UMDF2 minidriver.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
        public static int HMBridge_ReleaseAllNeutral()
        {
            try
            {
                return ReleaseAllNeutralInternal(bestEffortBounded: false);
            }
            catch
            {
                return -1;
            }
        }

        /// <summary>
        /// Emergency Driver-Level Teardown & Orphan Sweep:
        /// 1. Sends neutral release frames to both virtual controllers.
        /// 2. Disposes both virtual controllers in parallel.
        /// 3. Disposes HMContext.
        /// 4. If emergencyScopeEverything != 0, calls HMContext.RemoveAllVirtualControllers(HMCleanupScope.Everything, true).
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
        public static int HMBridge_EmergencyTeardown(int emergencyScopeEverything)
        {
            s_initialized = false;
            try
            {
                ReleaseAllNeutralInternal(bestEffortBounded: true);
            }
            catch
            {
                // Ignore during emergency teardown
            }

            lock (s_lifecycleLock)
            {
                try
                {
                    HMController? mouse = null;
                    bool mLocked = false;
                    try
                    {
                        mLocked = TryAcquireSpinGuardBounded(ref s_mouseGuard, 20000);
                        mouse = Interlocked.Exchange(ref s_mouseController, null);
                    }
                    finally
                    {
                        if (mLocked)
                        {
                            ReleaseSpinGuard(ref s_mouseGuard);
                        }
                    }

                    HMController? kb = null;
                    bool kLocked = false;
                    try
                    {
                        kLocked = TryAcquireSpinGuardBounded(ref s_keyboardGuard, 20000);
                        kb = Interlocked.Exchange(ref s_keyboardController, null);
                    }
                    finally
                    {
                        if (kLocked)
                        {
                            ReleaseSpinGuard(ref s_keyboardGuard);
                        }
                    }

                    s_liveMouseProfileId = null;
                    s_liveKeyboardProfileId = null;
                    s_mouseFaulted = false;
                    s_keyboardFaulted = false;

                    if (s_context != null && (mouse != null || kb != null))
                    {
                        var list = new List<HMController>(2);
                        if (mouse != null) list.Add(mouse);
                        if (kb != null) list.Add(kb);
                        s_context.DisposeControllersInParallel(list, null);
                    }
                    else
                    {
                        mouse?.Dispose();
                        kb?.Dispose();
                    }

                    s_context?.Dispose();
                    s_context = null;
                    s_driverInstalledCached = false;
                    s_mouseTransportIndex = -1;
                    s_keyboardTransportIndex = -1;
                    s_activeMouseVid = 0;
                    s_activeMousePid = 0;
                    s_activeKeyboardVid = 0;
                    s_activeKeyboardPid = 0;

                    if (IsProcessElevated())
                    {
                        HMCleanupScope scope = (emergencyScopeEverything != 0)
                            ? HMCleanupScope.Everything
                            : HMCleanupScope.Orphaned;
                        HMContext.RemoveAllVirtualControllers(scope, preserveInstall: true);
                    }

                    s_lastStatus = "HIDMaestro Virtual Controllers Safely Released & Torn Down.";
                    return 0;
                }
                catch (Exception ex)
                {
                    s_lastStatus = $"HIDMaestro Teardown Warning: {ex.Message}";
                    return -1;
                }
            }
        }

        /// <summary>
        /// Queries live driver/controller telemetry for the C++23 UI & Watchdog.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
        public static void HMBridge_QueryTelemetry(
            int* outInitialized,
            int* outElevated,
            int* outDriverInstalled,
            int* outMouseTransportIdx,
            int* outKbTransportIdx,
            int* outDeliversEveryReport,
            int* outReleasesOnExit,
            int* outKeyboardLeds)
        {
            bool lockTaken = false;
            try
            {
                Monitor.TryEnter(s_lifecycleLock, 0, ref lockTaken);

                bool bothLive = s_initialized &&
                                Volatile.Read(ref s_mouseController) != null &&
                                Volatile.Read(ref s_keyboardController) != null;
                if (outInitialized != null) *outInitialized = bothLive ? 1 : 0;
                if (outElevated != null) *outElevated = IsProcessElevated() ? 1 : 0;
                if (outMouseTransportIdx != null) *outMouseTransportIdx = s_mouseTransportIndex;
                if (outKbTransportIdx != null) *outKbTransportIdx = s_keyboardTransportIndex;

                if (lockTaken)
                {
                    HMContext? ctx = s_context;
                    HMController? mouse = Volatile.Read(ref s_mouseController);
                    HMController? kb = Volatile.Read(ref s_keyboardController);
                    if (outDriverInstalled != null) *outDriverInstalled = (ctx != null && ctx.IsDriverInstalled) ? 1 : 0;
                    if (outDeliversEveryReport != null)
                    {
                        *outDeliversEveryReport = ((mouse != null && mouse.DeliversEveryReport) && (kb != null && kb.DeliversEveryReport)) ? 1 : 0;
                    }
                    if (outReleasesOnExit != null)
                    {
                        *outReleasesOnExit = ((mouse != null && mouse.ReleasesOnConsumerExit) && (kb != null && kb.ReleasesOnConsumerExit)) ? 1 : 0;
                    }
                    if (outKeyboardLeds != null)
                    {
                        HMKeyboardLeds? leds = kb?.KeyboardLeds;
                        *outKeyboardLeds = leds.HasValue ? (int)leds.Value : 0;
                    }
                }
                else
                {
                    if (outDriverInstalled != null) *outDriverInstalled = s_driverInstalledCached ? 1 : 0;
                    if (outDeliversEveryReport != null) *outDeliversEveryReport = 0;
                    if (outReleasesOnExit != null) *outReleasesOnExit = 0;
                    if (outKeyboardLeds != null) *outKeyboardLeds = 0;
                }
            }
            catch
            {
                if (outInitialized != null) *outInitialized = 0;
                if (outElevated != null) *outElevated = 0;
                if (outDriverInstalled != null) *outDriverInstalled = 0;
                if (outMouseTransportIdx != null) *outMouseTransportIdx = -1;
                if (outKbTransportIdx != null) *outKbTransportIdx = -1;
                if (outDeliversEveryReport != null) *outDeliversEveryReport = 0;
                if (outReleasesOnExit != null) *outReleasesOnExit = 0;
                if (outKeyboardLeds != null) *outKeyboardLeds = 0;
            }
            finally
            {
                if (lockTaken)
                {
                    Monitor.Exit(s_lifecycleLock);
                }
            }
        }

        /// <summary>
        /// Returns the active virtual mouse and keyboard USB VID/PIDs so GameInput can filter them cleanly.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
        public static void HMBridge_GetActiveHardwareIds(
            ushort* outMouseVid,
            ushort* outMousePid,
            ushort* outKbVid,
            ushort* outKbPid)
        {
            try
            {
                if (outMouseVid != null) *outMouseVid = unchecked((ushort)s_activeMouseVid);
                if (outMousePid != null) *outMousePid = unchecked((ushort)s_activeMousePid);
                if (outKbVid != null) *outKbVid = unchecked((ushort)s_activeKeyboardVid);
                if (outKbPid != null) *outKbPid = unchecked((ushort)s_activeKeyboardPid);
            }
            catch
            {
                if (outMouseVid != null) *outMouseVid = 0;
                if (outMousePid != null) *outMousePid = 0;
                if (outKbVid != null) *outKbVid = 0;
                if (outKbPid != null) *outKbPid = 0;
            }
        }
    }
}
