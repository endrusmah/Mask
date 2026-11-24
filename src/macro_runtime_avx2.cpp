#include "macro_runtime.hpp"

#include <immintrin.h>

namespace mask::runtime {

uint64_t EvaluateExpiredMaskAvx2Bmi2Impl(
    const uint64_t* deadlines,
    uint64_t activeMask,
    uint64_t currentTsc
) noexcept {
    if (activeMask == 0ULL || deadlines == nullptr) {
        return 0ULL;
    }

    const __m256i vSignFlip   = _mm256_set1_epi64x(static_cast<long long>(0x8000000000000000ULL));
    const __m256i vNowFlipped = _mm256_xor_si256(
        _mm256_set1_epi64x(static_cast<long long>(currentTsc)),
        vSignFlip);

    auto evalGroup = [&](size_t group) noexcept -> uint64_t {
        const __m256i vDead        = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(deadlines + (group * 4)));
        const __m256i vDeadFlipped = _mm256_xor_si256(vDead, vSignFlip);
        // Unsigned 64-bit comparison: now >= dead  <=>  !(dead > now)
        const __m256i vDeadGtNow   = _mm256_cmpgt_epi64(vDeadFlipped, vNowFlipped);
        const uint32_t gtMask4 = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(vDeadGtNow))) & 0xFU;
        const uint32_t geMask4 = (~gtMask4) & 0xFU;
        return static_cast<uint64_t>(geMask4) << (group * 4);
    };

    if ((activeMask >> 4) == 0ULL) {
        return evalGroup(0) & activeMask;
    }

    uint64_t expiredMask = 0ULL;
    for (size_t group = 0; group < 16; ++group) {
        if (((activeMask >> (group * 4)) & 0xFULL) == 0ULL) {
            continue;
        }
        expiredMask |= evalGroup(group);
    }

    return expiredMask & activeMask;
}

} // namespace mask::runtime
