#pragma once

// Modular Application Coordinator for Mask (`mask::app::App`).
// Owns all runtime subsystems, worker threads, Win32 window/tray lifecycle,
// command dispatch, settings/profile persistence, and self-test / benchmark runners.
// Zero global subsystem singletons.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core_contracts.hpp"
#include "timing_engine.hpp"
#include "hidmaestro_bridge.hpp"
#include "input_pipeline.hpp"
#include "macro_runtime.hpp"
#include "watchdog.hpp"
#include "app_settings.hpp"
#include "pixel_trigger_engine.hpp"
#include "complex_sequence_engine.hpp"
#include "remap_engine.hpp"
#include "ui_anim.hpp"
#include "ui_actions.hpp"
#include "app_model.hpp"
#include "app_view.hpp"
#include "profile_json.hpp"
#include "resource.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace mask::app {

struct LaunchOptions {
    bool selfTest{false};
    bool bench{false};
    bool relaunched{false};
    bool startMinimized{false};
};

struct PendingCommandApproval {
    uint8_t  seqIdx{0};
    uint16_t stepIdx{0};
    bool     isShell{false};
    wchar_t  text[128]{};
};

inline constexpr UINT_PTR kUiTimerId               = 1001;
inline constexpr UINT_PTR kBindGraceTimerId        = 1002;
inline constexpr UINT_PTR kAnimTimerId             = 1003; // 16ms ONLY while focused & animating (0% idle CPU)
inline constexpr UINT_PTR kTrimMemTimerId          = 1004; // 5s one-shot working-set trim when minimized
inline constexpr UINT_PTR kTriggerPreviewTimerId   = 1005; // 50ms ONLY when visible & viewing a Trigger Bot

inline constexpr UINT     kUiFocusedIntervalMs     = 250;
inline constexpr UINT     kUiBackgroundIntervalMs  = 1000;
inline constexpr UINT     kAnimIntervalMs          = 16;
inline constexpr UINT     kTrimMemDelayMs          = 5000;
inline constexpr UINT     kTrigLiveIntervalMs      = 50;
inline constexpr UINT     kTrigDropperIntervalMs   = 16;

inline constexpr UINT     WM_APP_ENGINE_STATE        = WM_APP + 43;
inline constexpr UINT     WM_APP_OUTPUT_STATE        = WM_APP + 44;
inline constexpr UINT     WM_APP_TRAY_ICON           = WM_APP + 45;
inline constexpr UINT     WM_APP_TOGGLE_MACRO_SLOT   = WM_APP + 46;
inline constexpr UINT     WM_APP_RESTORE_WINDOW      = WM_APP + 47;
inline constexpr UINT     WM_APP_APPROVE_IMPORTED_CMD = WM_APP + 48;

inline constexpr UINT     kTrayIconId              = 1;
inline constexpr UINT     kTrayCmdOpen             = 40001;
inline constexpr UINT     kTrayCmdPause            = 40003;
inline constexpr UINT     kTrayCmdStopAll          = 40004;
inline constexpr UINT     kTrayCmdExit             = 40005;

inline constexpr ULONGLONG kToastLifetimeMs        = 5500;
inline constexpr uint32_t  kOutputFailureLimit     = 5;
inline constexpr const wchar_t* kWindowClass       = L"MaskMacroEngineWindow";
inline constexpr const wchar_t* kWindowTitle       = L"Mask";
inline constexpr const wchar_t* kMutexName         = L"Local\\MaskMacroEngine.SingleInstance";

inline constexpr float kDefaultWidthDip            = 1160.0f;
inline constexpr float kDefaultHeightDip           = 760.0f;
inline constexpr float kMinWidthDip                = 980.0f;
inline constexpr float kMinHeightDip               = 690.0f;

class App {
public:
    App() noexcept = default;
    ~App() noexcept = default;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    [[nodiscard]] int Run(const LaunchOptions& opts) noexcept;

private:
    // Owned runtime subsystems (zero globals)
    mask::core::EngineSharedState            sharedState_{};
    mask::timing::TimingEngine               timingEngine_{};
    mask::app::AppModel                      model_{sharedState_};
    mask::ui::UiAnimator                     animator_{};
    mask::app::UiRenderer                    renderer_{};
    mask::triggers::PixelTriggerEngine       pixelEngine_{sharedState_};
    mask::workflows::ComplexSequenceEngine   seqEngine_{sharedState_, &mask::triggers::PixelTriggerEngine::SamplePixelAt};
    mask::remap::RemapEngine                 remapEngine_{sharedState_};
    std::unique_ptr<mask::safety::Watchdog>  watchdog_{};

    // Worker threads
    std::thread gameInputThread_{};
    std::thread engineThread_{};
    std::thread outputThread_{};
    std::thread outputWorker_{};

    // Cross-thread coordination flags
    std::atomic<bool> uiNotifyPending_{false};
    std::atomic<bool> settingsHwTabOpen_{false};
    std::atomic<bool> outputWorkerBusy_{false};
    std::atomic<bool> outputThreadStop_{false};

    // Thread-safe pending imported command approval mailbox (fixed 32-slot ring, zero heap allocations)
    SRWLOCK                                  approvalLock_ = SRWLOCK_INIT;
    std::array<PendingCommandApproval, 32>   approvalQueue_{};
    size_t                                   approvalHead_{0};
    size_t                                   approvalTail_{0};
    size_t                                   approvalCount_{0};
    bool                                     commandApprovalDialogActive_{false};

    // Window, tray, focus, timer, clipboard & UI sync state
    HWND      hwnd_{nullptr};
    HICON     iconLarge_{nullptr};
    HICON     iconSmall_{nullptr};
    bool      trayAdded_{false};
    bool      hiddenToTray_{false};
    bool      windowFocused_{true};
    bool      animTimerActive_{false};
    bool      trimTimerActive_{false};
    bool      settingsTabSnapped_{false};
    bool      sidebarIndSnapped_{false};
    UINT      uiTimerMs_{0};
    UINT      triggerPreviewTimerMs_{0};
    bool      previewActiveSent_{false};
    uint8_t   previewSlotSent_{0};

    uint32_t  lastUiPhysHeldMask_{0};
    uint64_t  lastUiTelemetrySeq_{0};
    uint16_t  lastActiveSeqMask_{0};
    uint8_t   lastActiveRemapLayer_{0};
    bool      lastRemapUnlocked_{true};
    bool      lastObservedPaused_{false};
    uint8_t   lastObservedLayer_{0};
    bool      lastObservedUnlocked_{true};
    uint64_t  lastFiringMask_{0};
    uint16_t  lastFiringSeqMask_{0};

    int       autoReconnectAttempts_{0};
    ULONGLONG nextAutoReconnectTick_{0};
    ULONGLONG suppressKeyNavUntil_{0};
    uint16_t  bindGraceVk_{0};
    ULONGLONG bindGraceUntil_{0};

    // ONLY for OS/engine static callback trampolines (HWND dispatch uses GWLP_USERDATA)
    static App* activeInstance_;

    // ---- Static OS & Engine Trampolines -------------------------------------------
    static LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
    static void OnWatchdogSafeStopStatic(mask::core::SafeStopReason reason) noexcept;
    static void OnWatchdogAuxCleanupStatic() noexcept;
    static void OnPhysicalDeviceTransitionStatic(const uint8_t deviceId[32], uint8_t deviceKind, uint16_t vk,
                                                 bool isDown, uint64_t nowTsc) noexcept;
    static void OnTriggerComplexSeqStatic(uint8_t sequenceIndex) noexcept;
    static void OnToggleMacroSlotFromSeqStatic(uint8_t slotIndex) noexcept;
    static void OnCommandApprovalStatic(uint8_t sequenceIndex, uint16_t stepIndex, bool isShellCommand,
                                        const wchar_t* commandText) noexcept;

    // ---- Window & Timers (`src/app_window.cpp`) -----------------------------------
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
    void EnsureAnimTimer(HWND hwnd) noexcept;
    void UpdateUiTimerCadence(HWND hwnd) noexcept;
    void UpdateTriggerPreviewState(HWND hwnd) noexcept;
    void OnAnimTimer(HWND hwnd) noexcept;
    void OnUiTimer(HWND hwnd) noexcept;
    void SaveCurrentWindowBounds(HWND hwnd) noexcept;
    [[nodiscard]] uint32_t SelectedSlotPhysHeldMask() const noexcept;
    [[nodiscard]] FrameState BuildFrameState(HWND hwnd) const noexcept;
    void PasteClipboardIntoEditBuffer(size_t maxLen) noexcept;
    void SampleTriggerEyedropperLoupe(int32_t cx, int32_t cy) noexcept;
    void CancelTriggerEyedropper(HWND hwnd) noexcept;
    void FinishTriggerEyedropperPick(HWND hwnd) noexcept;
    void QueueCommandApproval(uint8_t seqIdx, uint16_t stepIdx, bool isShell, const wchar_t* text) noexcept;
    void ProcessPendingCommandApprovals(HWND hwnd) noexcept;
    [[nodiscard]] int RunGuiLoop(const LaunchOptions& opts) noexcept;

    // ---- Chrome & System Tray (`src/app_tray.cpp`) --------------------------------
    void ApplyWin11Chrome(HWND hwnd) noexcept;
    void ApplyAlwaysOnTop(HWND hwnd) noexcept;
    void RefreshAppIcons(HWND hwnd) noexcept;
    void DestroyAppIcons() noexcept;
    void UpdateTrayTooltip(HWND hwnd) noexcept;
    void AddTrayIcon(HWND hwnd) noexcept;
    void RemoveTrayIcon(HWND hwnd) noexcept;
    void StartTrimTimerIfEnabled(HWND hwnd) noexcept;
    void CancelTrimTimer(HWND hwnd) noexcept;
    void HideToTray(HWND hwnd) noexcept;
    void RestoreFromTray(HWND hwnd) noexcept;
    void ShowTrayContextMenu(HWND hwnd) noexcept;

    // ---- Settings & Engine Sync (`src/app_settings_io.cpp`) -----------------------
    void NotifyUi() noexcept;
    void PlayFeedbackTone(bool high) noexcept;
    void ShowToast(HWND hwnd, const wchar_t* message, UiAction action = UiAction{},
                   const wchar_t* actionLabel = L"") noexcept;
    void SyncAnimatorToggles(bool snap) noexcept;
    void SyncAllEnginesFromModel() noexcept;
    void SyncSelectedSequence() noexcept;
    void SyncSelectedRemap() noexcept;
    void SyncSelectedTrigger() noexcept;
    void StopEverythingRunning() noexcept;
    void UpdateTriggerSuppression() noexcept;
    void StartOutputWorker() noexcept;
    void RestartAsAdministrator(HWND hwnd) noexcept;

    // ---- Command Dispatch & Editing (`src/app_commands.cpp`) ----------------------
    using CommandFn = void (App::*)(HWND, UiAction, bool, bool) noexcept;
    [[nodiscard]] static const std::array<CommandFn, static_cast<size_t>(ActionKind::Count)>& DispatchTable() noexcept;

    void DispatchAction(HWND hwnd, UiAction act, bool shift, bool ctrl) noexcept;
    void SelectItem(HWND hwnd, ItemKind kind, int index) noexcept;
    void SelectNeighbour(HWND hwnd, int direction) noexcept;
    void ToggleItemEnabled(HWND hwnd, ItemKind kind, int idx) noexcept;
    void FinalizeItemDelete(HWND hwnd, int deleteId) noexcept;
    void OpenSettingsAtTab(HWND hwnd, int tab) noexcept;
    void SetEditing(Field f) noexcept;
    void CancelEdit() noexcept;
    void CommitEdit() noexcept;
    void BeginEdit(Field f) noexcept;
    void StepField(Field f, int direction, bool coarse, bool fine) noexcept;
    void ApplyDutyFromPointer(int xPx) noexcept;
    void ApplyTriggerSliderFromPointer(int xPx) noexcept;
    void CancelBinding() noexcept;
    void StartBinding(HWND hwnd, uint8_t target, const wchar_t* message) noexcept;
    void BeginBindGrace(HWND hwnd, uint16_t vk) noexcept;
    void OnBindGraceTimer(HWND hwnd) noexcept;
    [[nodiscard]] static UiAction BindActionFor(uint8_t target) noexcept;
    void AssignCapturedKey(HWND hwnd, uint16_t vk) noexcept;
    void OnKeyCaptured(HWND hwnd, uint16_t vk, bool fromMouse) noexcept;
    void EnsureSelectedSeqStepVisible() noexcept;

    // Command handlers
    void CmdNoOp(HWND, UiAction, bool, bool) noexcept;
    void CmdOpenNewPicker(HWND, UiAction, bool, bool) noexcept;
    void CmdCloseNewPicker(HWND, UiAction, bool, bool) noexcept;
    void CmdCreateAutoclicker(HWND, UiAction, bool, bool) noexcept;
    void CmdCreateSequence(HWND, UiAction, bool, bool) noexcept;
    void CmdCreateTrigger(HWND, UiAction, bool, bool) noexcept;
    void CmdCreateRemap(HWND, UiAction, bool, bool) noexcept;
    void CmdToggleSettings(HWND, UiAction, bool, bool) noexcept;
    void CmdOpenHardwareTab(HWND, UiAction, bool, bool) noexcept;
    void CmdSettingsTab(HWND, UiAction, bool, bool) noexcept;
    void CmdSidebarFilter(HWND, UiAction, bool, bool) noexcept;
    void CmdCycleTopLayer(HWND, UiAction, bool, bool) noexcept;
    void CmdToggleTopLock(HWND, UiAction, bool, bool) noexcept;
    void CmdTogglePause(HWND, UiAction, bool, bool) noexcept;
    void CmdStopAll(HWND, UiAction, bool, bool) noexcept;
    void CmdOutputAction(HWND, UiAction, bool, bool) noexcept;
    void CmdToastAction(HWND, UiAction, bool, bool) noexcept;
    void CmdSelectItemBase(HWND, UiAction, bool, bool) noexcept;
    void CmdToggleItemBase(HWND, UiAction, bool, bool) noexcept;

    // Settings command handlers
    void CmdSetToggleFlag(HWND, UiAction, bool, bool) noexcept;
    void CmdSetAnimQuality(HWND, UiAction, bool, bool) noexcept;
    void CmdSetAccent(HWND, UiAction, bool, bool) noexcept;
    void CmdSetPowerMode(HWND, UiAction, bool, bool) noexcept;
    void CmdSetPollHz(HWND, UiAction, bool, bool) noexcept;
    void CmdSetSpinPolicy(HWND, UiAction, bool, bool) noexcept;
    void CmdSetBindPauseVk(HWND, UiAction, bool, bool) noexcept;
    void CmdSetResetPauseVk(HWND, UiAction, bool, bool) noexcept;
    void CmdSetClearPauseVk(HWND, UiAction, bool, bool) noexcept;
    void CmdSetBindStopAllVk(HWND, UiAction, bool, bool) noexcept;
    void CmdSetClearStopAllVk(HWND, UiAction, bool, bool) noexcept;
    void CmdSetProfileBank(HWND, UiAction, bool, bool) noexcept;
    void CmdSetOpenDataFolder(HWND, UiAction, bool, bool) noexcept;
    void CmdSetResetAll(HWND, UiAction, bool, bool) noexcept;
    void CmdSetRefreshHw(HWND, UiAction, bool, bool) noexcept;
    void CmdExportProfileJson(HWND, UiAction, bool, bool) noexcept;
    void CmdImportProfileJson(HWND, UiAction, bool, bool) noexcept;
    void CmdSelectMouseProfile(HWND, UiAction, bool, bool) noexcept;
    void CmdSelectKbProfile(HWND, UiAction, bool, bool) noexcept;

    // Field & common item handlers
    void CmdFieldFocus(HWND, UiAction, bool, bool) noexcept;
    void CmdFieldDec(HWND, UiAction, bool, bool) noexcept;
    void CmdFieldInc(HWND, UiAction, bool, bool) noexcept;
    void CmdDeleteItem(HWND, UiAction, bool, bool) noexcept;
    void CmdToggleEnabled(HWND, UiAction, bool, bool) noexcept;
    void CmdDuplicateItem(HWND, UiAction, bool, bool) noexcept;

    // Autoclicker handlers
    void CmdBindTrigger(HWND, UiAction, bool, bool) noexcept;
    void CmdBindOutput(HWND, UiAction, bool, bool) noexcept;
    void CmdMode(HWND, UiAction, bool, bool) noexcept;
    void CmdQuickOut(HWND, UiAction, bool, bool) noexcept;
    void CmdHumanizeBase(HWND, UiAction, bool, bool) noexcept;

    // Sequence handlers
    void CmdSeqToggleStepKindMenu(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqBindTrigger(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepBindKey(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqTriggerMode(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqCycleHumanize(HWND, UiAction, bool, bool) noexcept;
    void CmdSelectSeqStep(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepAdd(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepInsert(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepDup(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepUp(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepDown(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepDel(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepKindBase(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepCondKind(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepCondNegate(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepCondUnlock(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepCondAddElse(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepCondQuickPct(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepLayer(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepQuickWait(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepQuickScroll(HWND, UiAction, bool, bool) noexcept;
    void CmdSeqStepPickColor(HWND, UiAction, bool, bool) noexcept;

    // Trigger handlers
    void CmdTrigPickPixel(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigLockCenter(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigBindArmVk(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigClearArmVk(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigBindActionVk(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigColorPreset(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigMode(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigMatchRule(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigRadius(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigMonitor(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigArmMode(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigActionKind(HWND, UiAction, bool, bool) noexcept;
    void CmdTrigQuickOut(HWND, UiAction, bool, bool) noexcept;

    // Remap handlers
    void CmdRemapLiveLayer(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapBindSource(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapBindTarget(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapCycleDevice(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapDevKind(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapMonitor(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapActionKind(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapRuleLayer(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapTargetLayer(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapUnlockPolicy(HWND, UiAction, bool, bool) noexcept;
    void CmdRemapToggleSuppress(HWND, UiAction, bool, bool) noexcept;

    // ---- Engine, Output & Input Threads (`src/app_engine_threads.cpp`) ------------
    void RunStartupPacingSelfTest() noexcept;
    void EngineThreadMain(bool selfTest, std::atomic<bool>* engineReadyFlag = nullptr) noexcept;
    void OutputThreadMain(std::atomic<bool>* outputReadyFlag = nullptr) noexcept;
    void GameInputThreadMain(std::atomic<bool>* inputReadyFlag) noexcept;

    // ---- Self-test & Benchmark (`src/app_selftest.cpp`) ---------------------------
    [[nodiscard]] int RunSelfTest() noexcept;
    [[nodiscard]] int RunBenchmark() noexcept;
};

} // namespace mask::app
