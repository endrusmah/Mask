#pragma once

#include "core_contracts.hpp"

#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstddef>
#include <array>
#include <limits>

namespace mask::timing {

/**
 * @brief Deterministic Sub-Microsecond Timing Engine.
 * Calibrates hardware invariant TSC against QPC, manages a persistent high-resolution kernel waitable timer,
 * boosts thread scheduling priority via MMCSS ("Pro Audio"), and provides hybrid sleep + _mm_pause() spin-waiting.
 */
class alignas(mask::core::kCacheLineSize) TimingEngine {
public:
    TimingEngine() noexcept;
    ~TimingEngine() noexcept;

    TimingEngine(const TimingEngine&) = delete;
    TimingEngine& operator=(const TimingEngine&) = delete;

    /**
     * @brief Reads the hardware Time Stamp Counter with pipeline serialization (__rdtscp)
     * and compiler read/write barriers so /O2 cannot hoist reads across kernel waits.
     */
    [[nodiscard]] static __forceinline uint64_t ReadTsc() noexcept {
        unsigned int aux = 0;
        _ReadWriteBarrier();
        const uint64_t tsc = __rdtscp(&aux);
        _ReadWriteBarrier();
        return tsc;
    }

    /**
     * @brief Calibrates the hardware TSC frequency against QueryPerformanceCounter over a 50ms window.
     * Computes 128-bit Q32.32 fixed-point multipliers for zero-overhead integer conversions.
     */
    void CalibrateTscFrequency() noexcept;

    /**
     * @brief Sets the hybrid sleep-to-spin pacing policy at runtime.
     *  - 0 = Auto         (adaptive EMA: minSpinLead = 150us, maxSpinLead = 2.0ms)
     *  - 1 = MaxPrecision (minSpinLead = 600us, maxSpinLead = 2.5ms, spinLead >= 600us)
     *  - 2 = Eco          (minSpinLead = 60us,  maxSpinLead = 800us)
     */
    void SetSpinPolicy(uint32_t policy) noexcept;

    [[nodiscard]] uint32_t GetSpinPolicy() const noexcept {
        return spinPolicy_;
    }

    /**
     * @brief Boosts the calling thread to MMCSS "Pro Audio" and THREAD_PRIORITY_TIME_CRITICAL,
     * disabling dynamic priority decay and Windows 11 EcoQoS power throttling.
     */
    [[nodiscard]] static HANDLE BoostThreadToProAudio() noexcept;
    static void RevertThreadProAudio(HANDLE mmcssHandle) noexcept;

    bool BoostCurrentThreadRealtimePriority() noexcept;
    void RevertThreadRealtimePriority() noexcept;

    [[nodiscard]] __forceinline uint64_t NanosecondsToTsc(uint64_t nanoseconds) const noexcept {
        return MulShiftQ32(nanoseconds, nsToTscFixedQ32_);
    }

    [[nodiscard]] __forceinline uint64_t TscToNanoseconds(uint64_t tscCycles) const noexcept {
        return MulShiftQ32(tscCycles, tscToNsFixedQ32_);
    }

    [[nodiscard]] __forceinline uint64_t GetTscFrequencyHz() const noexcept {
        return tscFrequencyHz_;
    }

    /**
     * @brief Hybrid Wait-Free Pacing until targetTsc.
     */
    uint64_t WaitUntilTsc(uint64_t targetTsc) noexcept;

    /**
     * @brief Event-aware hybrid wait used by the engine loop.
     * @return true if targetTsc was reached, false if woken early (caller should re-run its loop).
     */
    bool WaitUntilTscOrEvent(uint64_t targetTsc, HANDLE wakeEvent, uint64_t maxSliceNs) noexcept;

    /**
     * @brief Records a dispatch jitter sample (actualTsc - targetTsc) into a fixed 256-sample ring buffer
     * and 4-bucket latency histogram (<100ns, 100-500ns, 500-2000ns, >=2000ns) without heap allocations.
     */
    void RecordJitterSampleTsc(uint64_t targetTsc, uint64_t actualTsc) noexcept;

    void GetJitterHistogram(uint32_t outBuckets[4]) const noexcept;
    void ComputeJitterPercentiles(uint64_t& outP50Ns, uint64_t& outP99Ns, uint64_t& outMaxNs) const noexcept;

private:
    [[nodiscard]] static __forceinline uint64_t MulShiftQ32(uint64_t value, uint64_t fixedQ32) noexcept {
        uint64_t high = 0;
        uint64_t low  = _umul128(value, fixedQ32, &high);
        low += 0x8000'0000ULL;
        if (low < 0x8000'0000ULL) {
            ++high;
        }
        if ((high >> 32) != 0ULL) [[unlikely]] {
            return (std::numeric_limits<uint64_t>::max)();
        }
        return (high << 32) | (low >> 32);
    }

    [[nodiscard]] static uint64_t ComputeFixedQ32Ratio(uint64_t numerator, uint64_t denominator, uint64_t fallback) noexcept;
    void UpdateSpinLead(uint64_t lateTsc) noexcept;

    HANDLE   waitableTimer_{nullptr};
    bool     raisedTimerPeriod_{false};
    bool     spinPolicyApplied_{false};
    uint32_t spinPolicy_{0};
    uint64_t minSpinLeadTsc_{450'000ULL};
    uint64_t maxSpinLeadTsc_{6'000'000ULL};
    uint64_t leadMarginTsc_{300'000ULL};
    uint64_t minSleepTsc_{300'000ULL};
    uint64_t lateEmaTsc_{1'200'000ULL};
    HANDLE   mmcssHandle_{nullptr};
    uint64_t tscFrequencyHz_{3'000'000'000ULL};
    uint64_t nsToTscFixedQ32_{3ULL << 32};
    uint64_t tscToNsFixedQ32_{1'431'655'765ULL};
    uint64_t sleepThresholdTsc_{4'500'000ULL};
    uint64_t spinLeadTimeTsc_{3'000'000ULL};

    // Pre-allocated jitter telemetry ring buffer (256 samples) and 4-bucket histogram
    std::array<uint64_t, 256> jitterRing_{};
    std::array<uint32_t, 4>   jitterHistogram_{};
    size_t   jitterWriteIdx_{0};
    uint64_t maxJitterNs_{0};
};

} // namespace mask::timing
