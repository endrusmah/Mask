#include "app.hpp"

#include <shellapi.h>

#include <algorithm>
#include <array>

namespace mask::app {

void App::OnWatchdogSafeStopStatic(mask::core::SafeStopReason /*reason*/) noexcept {
    if (activeInstance_ != nullptr) {
        activeInstance_->NotifyUi();
    }
}

void App::OnWatchdogAuxCleanupStatic() noexcept {
    if (activeInstance_ == nullptr) return;
    activeInstance_->remapEngine_.ResetStateAndSuppression();
    activeInstance_->seqEngine_.AbortAll();
    activeInstance_->pixelEngine_.ResetArmedStates();
}

void App::OnPhysicalDeviceTransitionStatic(
    const uint8_t deviceId[32],
    uint8_t deviceKind,
    uint16_t vk,
    bool isDown,
    uint64_t nowTsc
) noexcept {
    if (activeInstance_ == nullptr) return;
    activeInstance_->remapEngine_.OnPhysicalDeviceTransition(deviceId, deviceKind, vk, isDown, nowTsc);
    activeInstance_->seqEngine_.NotifyPhysicalKeyTransition(vk, isDown);
}

void App::OnTriggerComplexSeqStatic(uint8_t sequenceIndex) noexcept {
    if (activeInstance_ == nullptr) return;
    activeInstance_->seqEngine_.TriggerSequence(sequenceIndex);
    activeInstance_->NotifyUi();
}

void App::OnToggleMacroSlotFromSeqStatic(uint8_t slotIndex) noexcept {
    const HWND hwnd = mask::input::GameInputGlobalContext::uiWindowHandle.load(std::memory_order_acquire);
    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_APP_TOGGLE_MACRO_SLOT, static_cast<WPARAM>(slotIndex), 0);
    }
}

void App::OnCommandApprovalStatic(
    uint8_t sequenceIndex,
    uint16_t stepIndex,
    bool isShellCommand,
    const wchar_t* commandText
) noexcept {
    if (activeInstance_ == nullptr) return;
    activeInstance_->QueueCommandApproval(sequenceIndex, stepIndex, isShellCommand, commandText);
}

void App::NotifyUi() noexcept {
    const HWND hwnd = mask::input::GameInputGlobalContext::uiWindowHandle.load(std::memory_order_acquire);
    if (hwnd == nullptr) return;
    if (!uiNotifyPending_.exchange(true, std::memory_order_acq_rel)) {
        if (!PostMessageW(hwnd, WM_APP_ENGINE_STATE, 0, 0)) {
            uiNotifyPending_.store(false, std::memory_order_release);
        }
    }
}

void App::PlayFeedbackTone(bool high) noexcept {
    if (!model_.settings.soundFeedback) return;
    MessageBeep(high ? MB_OK : MB_ICONASTERISK);
}

void App::ShowToast(HWND hwnd, const wchar_t* message, UiAction action, const wchar_t* actionLabel) noexcept {
    model_.SetToast(message, action, actionLabel);
    animator_.SetToastVisible(true);
    EnsureAnimTimer(hwnd);
}

void App::SyncAnimatorToggles(bool snap) noexcept {
    for (int i = 0; i < kSlotCount; ++i) {
        animator_.SetToggle(AnimSlotFor(ItemKind::Autoclicker, i), model_.slots[static_cast<size_t>(i)].enabled, snap);
    }
    for (int i = 0; i < kComplexSeqCount; ++i) {
        animator_.SetToggle(AnimSlotFor(ItemKind::Sequence, i), model_.sequences[static_cast<size_t>(i)].enabled, snap);
    }
    for (int i = 0; i < kColorTriggerCount; ++i) {
        animator_.SetToggle(AnimSlotFor(ItemKind::Trigger, i), model_.triggers[static_cast<size_t>(i)].enabled, snap);
    }
    for (int i = 0; i < kRemapRuleCount; ++i) {
        animator_.SetToggle(AnimSlotFor(ItemKind::Remap, i), model_.remaps[static_cast<size_t>(i)].enabled, snap);
    }
}

void App::SyncAllEnginesFromModel() noexcept {
    model_.SyncAllSlotsToEngine();
    seqEngine_.UpdateAllConfigs(model_.sequences);
    pixelEngine_.UpdateAllConfigs(model_.triggers);
    remapEngine_.UpdateAllRules(model_.remaps);
    model_.PublishWatchedKeys();
}

void App::SyncSelectedSequence() noexcept {
    if (model_.selectedKind != ItemKind::Sequence) return;
    const int idx = std::clamp(model_.selectedIndex, 0, kComplexSeqCount - 1);
    seqEngine_.UpdateConfig(static_cast<size_t>(idx), model_.sequences[static_cast<size_t>(idx)]);
    model_.PublishWatchedKeys();
    model_.MarkDirty();
}

void App::SyncSelectedRemap() noexcept {
    if (model_.selectedKind != ItemKind::Remap) return;
    const int idx = std::clamp(model_.selectedIndex, 0, kRemapRuleCount - 1);
    remapEngine_.UpdateRule(static_cast<size_t>(idx), model_.remaps[static_cast<size_t>(idx)]);
    model_.PublishWatchedKeys();
    model_.MarkDirty();
}

void App::SyncSelectedTrigger() noexcept {
    if (model_.selectedKind != ItemKind::Trigger) return;
    const int idx = std::clamp(model_.selectedIndex, 0, kColorTriggerCount - 1);
    pixelEngine_.UpdateConfig(static_cast<size_t>(idx), model_.triggers[static_cast<size_t>(idx)]);
    model_.PublishWatchedKeys();
    model_.MarkDirty();
}

void App::StopEverythingRunning() noexcept {
    sharedState_.flushAllRequested.store(true, std::memory_order_release);
    remapEngine_.ResetStateAndSuppression();
    seqEngine_.AbortAll();
    pixelEngine_.ResetArmedStates();
    mask::input::WakeEngine();
    if (watchdog_) {
        watchdog_->ReleaseAllSyntheticHeldInputs();
    }
}

void App::UpdateTriggerSuppression() noexcept {
    const bool editSuppress = model_.settings.suppressWhileTypingInApp && (model_.editField != Field::None);
    const bool suppress = editSuppress || (bindGraceVk_ != 0U);
    sharedState_.suppressTriggers.store(suppress, std::memory_order_release);
    mask::input::WakeEngine();
}

void App::StartOutputWorker() noexcept {
    if (outputWorkerBusy_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (outputWorker_.joinable()) {
        outputWorker_.join();
    }
    std::array<wchar_t, 64> mouseId{};
    std::array<wchar_t, 64> kbId{};
    wcsncpy_s(mouseId.data(), mouseId.size(), model_.mouseProfileId, _TRUNCATE);
    wcsncpy_s(kbId.data(), kbId.size(), model_.keyboardProfileId, _TRUNCATE);
    model_.outputState = OutputState::Starting;
    outputWorker_ = std::thread([this, mouseId, kbId]() noexcept {
        auto& host = mask::bridge::HIDMaestroHost::Instance();
        const bool loaded = host.LoadAndInitialize(mouseId.data(), kbId.data());
        uint16_t mVid = 0, mPid = 0, kVid = 0, kPid = 0;
        host.GetActiveVirtualHardwareIds(mVid, mPid, kVid, kPid);
        mask::input::RegisterVirtualDeviceVidPid(mVid, mPid);
        mask::input::RegisterVirtualDeviceVidPid(kVid, kPid);
        OutputState state = OutputState::Unavailable;
        if (host.AreControllersLive()) {
            state = OutputState::Ready;
        } else if (loaded) {
            state = OutputState::NeedsAdmin;
        }
        sharedState_.outputFailureStreak.store(0U, std::memory_order_release);
        outputWorkerBusy_.store(false, std::memory_order_release);
        const HWND hwnd = mask::input::GameInputGlobalContext::uiWindowHandle.load(std::memory_order_acquire);
        if (hwnd != nullptr) {
            PostMessageW(hwnd, WM_APP_OUTPUT_STATE, static_cast<WPARAM>(state), 0);
        }
    });
}

void App::RestartAsAdministrator(HWND hwnd) noexcept {
    wchar_t exePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0U || len >= MAX_PATH) return;
    (void)model_.SaveToDisk();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.hwnd         = hwnd;
    sei.lpVerb       = L"runas";
    sei.lpFile       = exePath;
    sei.lpParameters = L"--relaunch";
    sei.nShow        = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else {
        ShowToast(hwnd, L"Mask wasn't restarted, so macros still can't send input.");
    }
}

} // namespace mask::app
