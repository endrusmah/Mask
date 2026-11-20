#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <array>
#include <type_traits>
#include <new>

namespace mask::core {

inline constexpr size_t kCacheLineSize = 64;
inline constexpr size_t kMaxMacroSlots = 64;
inline constexpr size_t kMaxSequenceSteps = 8;
inline constexpr size_t kMaxComplexSequences = 16;
inline constexpr size_t kMaxComplexSteps = 128;
inline constexpr size_t kMaxColorTriggers = 8;
inline constexpr size_t kMaxRemapRules = 32;
inline constexpr size_t kMaxRemapLayers = 4;
inline constexpr uint64_t kMinimumPulseFloorNs = 50'000ULL; // 50 microseconds floor guard
inline constexpr uint16_t kVkLeftButton   = 0x0001U;        // VK_LBUTTON (avoids <windows.h> dependency here)
inline constexpr uint16_t kVkDefaultPause = 0x0077U;        // VK_F8
inline constexpr uint16_t kVkWheelUp      = 0x00F1U;        // Output-only pseudo-VK for mouse wheel up tick
inline constexpr uint16_t kVkWheelDown    = 0x00F2U;        // Output-only pseudo-VK for mouse wheel down tick

// A release ("recovery") time of exactly 0 means a true 100% duty cycle: the output is pressed once and
// stays down, with no release events at all, until the macro stops. Any other value is floored to 50 us.
[[nodiscard]] constexpr uint64_t ClampPressNs(uint64_t ns) noexcept {
    return ns < kMinimumPulseFloorNs ? kMinimumPulseFloorNs : ns;
}
[[nodiscard]] constexpr uint64_t ClampReleaseNs(uint64_t ns) noexcept {
    return ns == 0ULL ? 0ULL : (ns < kMinimumPulseFloorNs ? kMinimumPulseFloorNs : ns);
}

// Why the watchdog last forced everything to stop (shown in the UI).
enum class SafeStopReason : uint8_t {
    None        = 0,
    PanicChord  = 1, // L-Ctrl + R-Ctrl + Esc
    EngineStall = 2  // engine thread stopped making progress
};

/**
 * @brief Complete 5 Execution Modes per Macro Slot.
 */
enum class MacroMode : uint8_t {
    HoldToLoop         = 0, // Hold-to-Loop (While Held): Split-Phase Dwell -> Recovery while trigger held
    InfiniteToggleLoop = 1, // Infinite Toggle Loop (On/Off Toggle): Press once to start, press again to stop cleanly
    RepeatNTimes       = 2, // Repeat N Times (Burst Mode): Press once to execute repeatCount cycles and stop
    Sequence           = 3, // Multi-Key Sequence Mode: Executes up to 8 customizable steps (key/button + dwell/recovery)
    SocdPair           = 4  // SOCD (Snap-Tap / Last-Input Priority): Pairs triggerVk and outputVk with zero neutral dead-time
};

enum class SlotPhase : uint8_t {
    IDLE                  = 0,
    PHASE_1_DWELL_ACTIVE  = 1,
    PHASE_2_RECOVERY_WAIT = 2
};

/**
 * @brief Customizable step inside a Multi-Key Sequence (up to 8 steps per macro slot).
 */
struct SequenceStepConfig {
    uint64_t dwellDurationNs{15'000'000ULL};    // Step KeyDown hold time in ns (default 15,000 us)
    uint64_t recoveryDurationNs{20'000'000ULL}; // Step KeyUp separation delay in ns; 0 = next step starts with no gap
    uint16_t outputVk{'1'};                     // Virtual key or mouse button VK emitted at this step
    bool     isMouse{false};                    // True if outputVk is a mouse button (VK_LBUTTON..VK_XBUTTON2)
    bool     enabled{true};                     // Whether this step is active
    uint8_t  reserved[4]{0, 0, 0, 0};
};

// Data-Oriented Input Trigger Event pushed from Microsoft GameInput to Engine Thread
struct alignas(16) InputTriggerEvent {
    uint64_t timestampTsc{0};
    uint16_t vkCode{0};
    bool     isDown{false};
    uint8_t  reserved[5]{0, 0, 0, 0, 0};
};

// Data-Oriented Macro Configuration pushed from UI Thread to Engine Thread and serialized to disk
struct alignas(64) MacroConfig {
    uint64_t dwellDurationNs{5'000'000ULL};    // Phase 1 KeyDown/ButtonDown hold duration (ns) - default 5000 us
    uint64_t recoveryDurationNs{5'000'000ULL}; // Phase 2 KeyUp/ButtonUp separation duration (ns); 0 = 100% duty cycle
    int32_t  mouseDeltaX{0};                   // Optional per-cycle un-coalesced mouse X delta
    int32_t  mouseDeltaY{0};                   // Optional per-cycle un-coalesced mouse Y delta
    uint32_t repeatCount{10};                  // Burst count for RepeatNTimes mode (1..9999)
    uint16_t triggerVk{0};                     // Primary trigger Virtual Key (or Primary SOCD key)
    uint16_t outputVk{0};                      // Output Virtual Key / Mouse button VK (or Opposing SOCD key)
    uint8_t  mode{0};                          // Maps to MacroMode (0..4)
    bool     isMouse{false};                   // True if output dispatches mouse button/movement
    uint8_t  slotIndex{0};                     // 0..63 explicit slot index
    bool     enabled{false};                   // Master Enable/Disable switch for this slot
    uint8_t  stepCount{4};                     // Number of active steps in Sequence mode (1..8)
    uint8_t  reserved[3]{0, 0, 0};             // [0] = 1 once created (UI bookkeeping), [1] = humanizePct (0..50)
    wchar_t  name[24]{L"Macro Slot"};          // Human-readable slot label
    std::array<SequenceStepConfig, kMaxSequenceSteps> steps{};
};

static_assert(sizeof(MacroConfig) == 320, "MacroConfig size must remain 320 bytes for profiles.bin compatibility.");
static_assert(std::is_trivially_copyable_v<MacroConfig>, "MacroConfig must remain trivially copyable.");

[[nodiscard]] constexpr uint8_t HumanizePercent(const MacroConfig& c) noexcept {
    return c.reserved[1];
}
constexpr void SetHumanizePercent(MacroConfig& c, uint8_t pct) noexcept {
    c.reserved[1] = pct;
}

// =====================================================================================
//  Auxiliary Lock-Free Output / Workflow Command Contract
// =====================================================================================

enum class AuxActionKind : uint8_t {
    KeyDown           = 0,
    KeyUp             = 1,
    KeyClick          = 2,
    MouseMoveRel      = 3,
    MouseMoveAbs      = 4,
    MouseScroll       = 5,
    SetLayer          = 6,
    ToggleUnlock      = 7,
    TriggerMacroSlot  = 8,
    TriggerComplexSeq = 9
};

struct alignas(32) AuxOutputCommand {
    uint64_t timestampTsc{0};
    int32_t  argX{0};
    int32_t  argY{0};
    uint32_t holdMs{0};
    uint16_t vk{0};
    uint8_t  kind{0};
    uint8_t  auxIndex{0};
    uint8_t  reserved[4]{0, 0, 0, 0};
};

static_assert(sizeof(AuxOutputCommand) == 32, "AuxOutputCommand must remain 32 bytes.");
static_assert(std::is_trivially_copyable_v<AuxOutputCommand>, "AuxOutputCommand must be trivially copyable.");

// =====================================================================================
//  Complex Sequences (Multi-Step Workflow Automation)
// =====================================================================================

enum class ComplexStepKind : uint8_t {
    KeyClick      = 0,
    KeyDown       = 1,
    KeyUp         = 2,
    MouseMoveRel  = 3,
    MouseMoveAbs  = 4,
    MouseScroll   = 5,
    WaitMs        = 6,
    WaitForColor  = 7,
    LoopStart     = 8,
    LoopEnd       = 9,
    LaunchProgram = 10,
    RunCommand    = 11,
    SetLayer      = 12,
    ToggleMacro   = 13
};

struct ComplexStep {
    uint8_t  kind{0};
    uint8_t  flags{0};
    uint16_t vk{0};
    int32_t  x{0};
    int32_t  y{0};
    uint32_t durationMs{20};
    uint32_t param{0};       // loop count / monitor index / layer / macro slot / scroll delta / timeout ms
    uint8_t  r{0};
    uint8_t  g{0};
    uint8_t  b{0};
    uint8_t  tolerance{20};
    wchar_t  text[128]{};    // program path or command string
};

static_assert(std::is_trivially_copyable_v<ComplexStep>, "ComplexStep must be trivially copyable.");

struct ComplexSequenceConfig {
    uint8_t  slotIndex{0};
    bool     enabled{false};
    bool     inUse{false};
    uint8_t  triggerMode{0}; // 0=RunOnce, 1=ToggleLoop, 2=WhileHeld
    uint16_t triggerVk{0};
    uint16_t stepCount{0};
    uint8_t  humanizePct{0};
    uint8_t  reserved[3]{0, 0, 0};
    wchar_t  name[32]{L"Sequence"};
    std::array<ComplexStep, kMaxComplexSteps> steps{};
};

static_assert(std::is_trivially_copyable_v<ComplexSequenceConfig>, "ComplexSequenceConfig must be trivially copyable.");

// =====================================================================================
//  Pixel / Color Triggers (Ring-3 DXGI/GDI Screen Color Automation)
// =====================================================================================

enum class ColorTriggerMode : uint8_t {
    MatchColor   = 0,
    ColorChanged = 1
};

enum class ColorTriggerActionKind : uint8_t {
    ClickKey      = 0,
    HoldKey       = 1,
    RunMacroSlot  = 2,
    RunComplexSeq = 3
};

struct ColorTriggerConfig {
    uint8_t  slotIndex{0};
    bool     enabled{false};
    bool     inUse{false};
    uint8_t  mode{0};                 // ColorTriggerMode
    uint8_t  actionKind{0};           // ColorTriggerActionKind
    uint8_t  armMode{0};              // 0=AlwaysArmed, 1=WhileArmHeld, 2=ToggleArmed
    uint8_t  r{255};
    uint8_t  g{50};
    uint8_t  b{50};
    uint8_t  tolerance{28};
    uint8_t  monitorIndex{0};
    uint8_t  sampleRadius{2};
    uint16_t armVk{0};
    uint16_t actionVk{kVkLeftButton};
    int32_t  sampleX{-1};             // -1 = monitor center
    int32_t  sampleY{-1};
    uint32_t delayMs{0};
    uint32_t holdMs{15};
    uint32_t cooldownMs{80};
    uint8_t  linkedIndex{0};
    uint8_t  reserved[3]{0, 0, 0};
    wchar_t  name[32]{L"Color trigger"};
};

static_assert(std::is_trivially_copyable_v<ColorTriggerConfig>, "ColorTriggerConfig must be trivially copyable.");

// =====================================================================================
//  Multi-Device / Multi-Monitor Remapping
// =====================================================================================

enum class RemapUnlockPolicy : uint8_t {
    Always       = 0,
    WhenUnlocked = 1,
    WhenLocked   = 2
};

enum class RemapLayerAction : uint8_t {
    RemapKey          = 0,
    BlockKey          = 1,
    ShiftLayerHold    = 2,
    ToggleLayer       = 3,
    ToggleUnlockState = 4,
    WarpToMonitor     = 5
};

struct RemapRuleConfig {
    uint8_t  slotIndex{0};
    bool     enabled{false};
    bool     inUse{false};
    bool     suppressOriginal{false};
    bool     anyDevice{true};
    uint8_t  deviceKind{0};           // 0=Any, 1=Keyboard, 2=Mouse
    uint8_t  layerIndex{0};
    uint8_t  unlockPolicy{0};         // RemapUnlockPolicy
    uint8_t  actionKind{0};           // RemapLayerAction
    uint8_t  targetLayer{0};
    int8_t   monitorIndex{-1};        // -1=Any
    uint8_t  reserved{0};
    uint16_t sourceVk{0};
    uint16_t targetVk{0};
    uint8_t  deviceIdBytes[32]{};
    wchar_t  deviceLabel[40]{L"Any device"};
    wchar_t  name[32]{L"Remap rule"};
};

static_assert(std::is_trivially_copyable_v<RemapRuleConfig>, "RemapRuleConfig must be trivially copyable.");

// Telemetry snapshot exported lock-free from Engine Thread to UI Thread
struct alignas(64) TelemetrySnapshot {
    uint64_t calibratedTscFreqHz{0};
    uint64_t totalEventsDispatched{0};
    uint64_t totalHookEventsProcessed{0};
    uint64_t p50JitterNs{0};
    uint64_t p99JitterNs{0};
    uint64_t maxJitterNs{0};
    uint64_t engineLoopIterations{0};
    uint64_t activeSlotBitmask{0};             // Bit i = 1 if slot i is currently running
    uint64_t toggledSlotBitmask{0};            // Bit i = 1 if slot i is toggled ON
    uint32_t activeSlotCount{0};
    bool     avx2Bmi2Active{false};
    uint32_t gameInputDeviceCount{0};
    uint32_t jitterHistogram[4]{0, 0, 0, 0};   // <100ns, 100-500ns, 500ns-2us, >=2us
};

static_assert(std::is_trivially_copyable_v<TelemetrySnapshot>, "TelemetrySnapshot must remain trivially copyable.");

/**
 * @brief Lock-Free, Wait-Free Single-Producer Single-Consumer (SPSC) Ring Buffer.
 * Uses cache-line padding to eliminate false sharing between Producer and Consumer cores.
 * Capacity must be a power of two.
 */
template <typename T, size_t Capacity>
class alignas(kCacheLineSize) SPSCQueue {
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0),
                  "SPSCQueue Capacity must be a power of 2.");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCQueue requires T to be trivially copyable for zero-overhead lock-free transfer.");

public:
    constexpr SPSCQueue() noexcept = default;
    ~SPSCQueue() noexcept = default;

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    [[nodiscard]] bool try_push(const T& item) noexcept {
        const size_t currentTail = tail_.load(std::memory_order_relaxed);
        const size_t nextTail = (currentTail + 1) & kIndexMask;

        if (nextTail == headCache_) [[unlikely]] {
            headCache_ = head_.load(std::memory_order_acquire);
            if (nextTail == headCache_) [[unlikely]] {
                return false; // Queue full
            }
        }

        buffer_[currentTail] = item;
        tail_.store(nextTail, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& outItem) noexcept {
        const size_t currentHead = head_.load(std::memory_order_relaxed);

        if (currentHead == tailCache_) [[unlikely]] {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (currentHead == tailCache_) [[unlikely]] {
                return false; // Queue empty
            }
        }

        outItem = buffer_[currentHead];
        head_.store((currentHead + 1) & kIndexMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size_approx() const noexcept {
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t h = head_.load(std::memory_order_acquire);
        return (t - h) & kIndexMask;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t kIndexMask = Capacity - 1;

    // Producer-owned cache line
    alignas(kCacheLineSize) std::atomic<size_t> tail_{0};
    size_t headCache_{0};
    uint8_t padProducer_[kCacheLineSize - sizeof(std::atomic<size_t>) - sizeof(size_t)]{};

    // Consumer-owned cache line
    alignas(kCacheLineSize) std::atomic<size_t> head_{0};
    size_t tailCache_{0};
    uint8_t padConsumer_[kCacheLineSize - sizeof(std::atomic<size_t>) - sizeof(size_t)]{};

    // Data storage aligned to cache line boundary
    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

/**
 * @brief Lock-Free, Zero-Allocation Bounded Multi-Producer Single-Consumer (MPSC) Ring Buffer
 * (Vyukov per-slot sequence protocol with CAS on tail_ and wait-free single consumer).
 * Capacity must be a power of two >= 2.
 */
template <typename T, size_t Capacity>
class alignas(kCacheLineSize) MPSCQueue {
    static_assert((Capacity >= 2) && ((Capacity & (Capacity - 1)) == 0),
                  "MPSCQueue Capacity must be a power of 2 >= 2.");
    static_assert(std::is_trivially_copyable_v<T>,
                  "MPSCQueue requires T to be trivially copyable for zero-overhead lock-free transfer.");

    struct Cell {
        std::atomic<size_t> sequence{0};
        T data{};
    };

public:
    MPSCQueue() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
    ~MPSCQueue() noexcept = default;

    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    [[nodiscard]] bool try_push(const T& item) noexcept {
        Cell* cell = nullptr;
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & kIndexMask];
            const size_t seq = cell->sequence.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq - pos);
            if (dif == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false; // Queue full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& outItem) noexcept {
        const size_t pos = head_.load(std::memory_order_relaxed);
        Cell& cell = buffer_[pos & kIndexMask];
        const size_t seq = cell.sequence.load(std::memory_order_acquire);
        const intptr_t dif = static_cast<intptr_t>(seq - (pos + 1));
        if (dif < 0) {
            return false; // Queue empty or producer still publishing
        }
        outItem = cell.data;
        cell.sequence.store(pos + Capacity, std::memory_order_release);
        head_.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size_approx() const noexcept {
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t h = head_.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(t - h);
        return diff > 0 ? static_cast<size_t>(diff) : 0U;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t kIndexMask = Capacity - 1;

    // Producer-shared cache line (claimed via CAS by concurrent producers)
    alignas(kCacheLineSize) std::atomic<size_t> tail_{0};
    uint8_t padProducer_[kCacheLineSize - sizeof(std::atomic<size_t>)]{};

    // Consumer-owned cache line (advanced solely by the single consumer)
    alignas(kCacheLineSize) std::atomic<size_t> head_{0};
    uint8_t padConsumer_[kCacheLineSize - sizeof(std::atomic<size_t>)]{};

    // Data storage aligned to cache line boundary
    alignas(kCacheLineSize) std::array<Cell, Capacity> buffer_{};
};

/**
 * @brief Shared Engine State across GameInput Thread, Real-Time Engine Thread, Watchdog Thread, and UI Thread.
 * Strictly pre-allocated and cache-line segregated.
 */
struct alignas(kCacheLineSize) EngineSharedState {
    // Cache Line 0: Lifecycle, Watchdog Heartbeat & Interactive Key-Binding Capture State
    alignas(kCacheLineSize) std::atomic<uint64_t> heartbeatCounter{0};
    std::atomic<bool>     engineRunning{true};
    std::atomic<bool>     emergencyPanicTriggered{false}; // Set by the input thread on the panic chord; consumed by the watchdog
    std::atomic<bool>     automationPaused{false};
    std::atomic<bool>     flushAllRequested{false};
    std::atomic<bool>     gameInputConnected{false};
    std::atomic<bool>     suppressTriggers{false};    // True while the user types in a UI text field
    std::atomic<uint8_t>  captureBindTarget{0};       // 0 = None, 1 = TriggerVk, 2 = OutputVk, 3..10 = Sequence Step 0..7
    std::atomic<uint8_t>  safeStopReason{0};          // SafeStopReason of the most recent watchdog stop (0 once resumed)
    std::atomic<uint16_t> capturedVk{0};              // Written by GameInput or UI when captureBindTarget != 0
    std::atomic<uint32_t> captureSerial{0};           // Incremented every time a new key-capture session starts
    std::atomic<uint32_t> outputFailureStreak{0};     // Consecutive failed hardware report submissions
    std::atomic<uint64_t> lastSyntheticHardwareWriteTsc{0}; // Used by GameInput pipeline as secondary echo guard
    std::atomic<uint32_t> activeMouseButtonMask{0};         // Tracked HMMouseButtons bitmask currently held on virtual mouse
    std::atomic<uint8_t>  activeKeyModifierMask{0};         // Tracked HMKeyModifiers bitmask currently held on virtual keyboard
    std::atomic<uint64_t> telemetrySequence{0};
    TelemetrySnapshot     telemetry{};

    // Keys the input thread must verify by polling (macro triggers + built-in hotkeys). Written by the UI thread.
    alignas(kCacheLineSize) std::array<std::atomic<uint64_t>, 4> watchedVkBits{};
    std::atomic<bool>     anyMacroEnabled{false};

    // Global runtime policy, hotkeys, and remapping layer state (written by UI/Input, read across threads)
    alignas(kCacheLineSize) std::atomic<uint16_t> globalPauseVk{kVkDefaultPause};
    std::atomic<uint16_t> globalStopAllVk{0};
    std::atomic<uint8_t>  activeRemapLayer{0};
    std::atomic<bool>     remapUnlocked{true};
    std::atomic<bool>     lowPowerUiMode{false};
    std::atomic<uint32_t> armedPollIntervalUs{1000};
    std::atomic<uint32_t> spinPolicy{0};              // 0=Auto, 1=MaxPrecision, 2=Eco
    std::atomic<bool>     panicChordEnabled{true};
    std::atomic<bool>     soundFeedbackEnabled{false};

    // Suppressed physical VK bitmask (for remap rules with suppressOriginal / BlockKey)
    alignas(kCacheLineSize) std::array<std::atomic<uint64_t>, 4> suppressedVkBits{};
    std::atomic<bool>     anySuppressionActive{false};

    // Cache Line 1+: Physical Key/Button State (Updated by GameInput hardware stream, read by Engine)
    alignas(kCacheLineSize) std::array<std::atomic<uint8_t>, 256> physicalKeyState{};

    // Cache Line 5+: Synthetic Held State (Updated by HIDMaestro Dispatcher BEFORE hardware write, read by Watchdog on emergency)
    alignas(kCacheLineSize) std::array<std::atomic<uint8_t>, 256> syntheticHeldState{};

    // Lock-Free Queues (MPSC for multi-producer input/aux feeds; SPSC for UI->Engine macro config sync)
    MPSCQueue<InputTriggerEvent, 4096> inputEventQueue{};
    SPSCQueue<MacroConfig, 256>        uiCommandQueue{};
    MPSCQueue<AuxOutputCommand, 1024>  auxOutputQueue{};

    EngineSharedState() noexcept {
        for (size_t i = 0; i < 256; ++i) {
            physicalKeyState[i].store(0, std::memory_order_relaxed);
            syntheticHeldState[i].store(0, std::memory_order_relaxed);
        }
        for (auto& w : watchedVkBits) {
            w.store(0, std::memory_order_relaxed);
        }
        for (auto& w : suppressedVkBits) {
            w.store(0, std::memory_order_relaxed);
        }
    }

    void PublishTelemetry(const TelemetrySnapshot& snap) noexcept {
        const uint64_t seq = telemetrySequence.load(std::memory_order_relaxed);
        telemetrySequence.store(seq + 1, std::memory_order_release); // Odd = write in progress
        std::atomic_signal_fence(std::memory_order_acq_rel);
        telemetry = snap;
        std::atomic_signal_fence(std::memory_order_acq_rel);
        telemetrySequence.store(seq + 2, std::memory_order_release); // Even = consistent
    }

    [[nodiscard]] TelemetrySnapshot ReadTelemetry() const noexcept {
        TelemetrySnapshot local{};
        uint64_t seq0 = 0;
        uint64_t seq1 = 0;
        do {
            seq0 = telemetrySequence.load(std::memory_order_acquire);
            if (seq0 & 1ULL) {
                continue;
            }
            std::atomic_signal_fence(std::memory_order_acq_rel);
            local = telemetry;
            std::atomic_signal_fence(std::memory_order_acq_rel);
            seq1 = telemetrySequence.load(std::memory_order_acquire);
        } while (seq0 != seq1);
        return local;
    }
};

} // namespace mask::core
