#pragma once

// Zero-dependency UTF-8 JSON profile bank serializer & parser for Mask.
// Exports and imports Autoclickers/Macros, Complex Sequences, Color Triggers, and Remap Rules.
// Automatically marks imported Complex Sequences containing LaunchProgram/RunCommand steps
// with mask::core::kSeqFlagUntrustedImport so the runtime prompts before execution.

#include "core_contracts.hpp"

#include <array>
#include <string>

namespace mask::profile_json {

inline constexpr uint32_t kProfileJsonSchemaVersion = 2U;

[[nodiscard]] bool SerializeProfileBankToJson(
    const std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>& macros,
    const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& sequences,
    const std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& triggers,
    const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& remaps,
    std::string& outUtf8Json) noexcept;

[[nodiscard]] bool DeserializeProfileBankFromJson(
    const std::string& utf8Json,
    std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>& outMacros,
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& outSequences,
    std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& outTriggers,
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& outRemaps,
    bool& outHasImportedCommands) noexcept;

[[nodiscard]] bool ExportProfileBankToJsonFile(
    const wchar_t* path,
    const std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>& macros,
    const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& sequences,
    const std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& triggers,
    const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& remaps) noexcept;

[[nodiscard]] bool ImportProfileBankFromJsonFile(
    const wchar_t* path,
    std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>& outMacros,
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& outSequences,
    std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& outTriggers,
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& outRemaps,
    bool& outHasImportedCommands) noexcept;

[[nodiscard]] bool RunProfileJsonSelfTest() noexcept;

} // namespace mask::profile_json
