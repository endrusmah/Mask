#include "profile_json.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace mask::profile_json {

namespace {

inline constexpr size_t kMaxJsonNestingDepth = 32U;
inline constexpr uint64_t kMaxJsonFileBytes  = 16ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr bool IsMouseVk(uint16_t vk) noexcept {
    const uint16_t b = static_cast<uint16_t>(vk & 0xFFU);
    return b == VK_LBUTTON || b == VK_RBUTTON || b == VK_MBUTTON ||
           b == VK_XBUTTON1 || b == VK_XBUTTON2 ||
           b == mask::core::kVkWheelUp || b == mask::core::kVkWheelDown;
}

[[nodiscard]] bool WideToUtf8(const wchar_t* src, std::string& out) {
    out.clear();
    if (src == nullptr || src[0] == L'\0') {
        return true;
    }
    const size_t wlen = std::wcslen(src);
    if (wlen > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, src, static_cast<int>(wlen), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        // Retry without strict invalid-char flag in case memory held a legacy character
        const int fallback = WideCharToMultiByte(
            CP_UTF8, 0U, src, static_cast<int>(wlen), nullptr, 0, nullptr, nullptr);
        if (fallback <= 0) {
            return false;
        }
        out.resize(static_cast<size_t>(fallback));
        return WideCharToMultiByte(
            CP_UTF8, 0U, src, static_cast<int>(wlen), out.data(), fallback, nullptr, nullptr) == fallback;
    }
    out.resize(static_cast<size_t>(needed));
    return WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, src, static_cast<int>(wlen), out.data(), needed, nullptr, nullptr) == needed;
}

[[nodiscard]] bool Utf8ToWideFixed(std::string_view utf8, wchar_t* dst, size_t cap) {
    if (dst == nullptr || cap == 0U) {
        return false;
    }
    std::memset(dst, 0, cap * sizeof(wchar_t));
    if (utf8.empty()) {
        return true;
    }
    // Truncate cleanly before Win32 call only on very long strings if needed, keeping UTF-8 boundary intact.
    if (utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
            wide.data(), needed) != needed) {
        return false;
    }
    const size_t maxCopy = cap - 1U;
    size_t copyLen = std::min(maxCopy, wide.size());
    // Do not split a UTF-16 surrogate pair across buffer truncation boundary.
    if (copyLen > 0U && copyLen < wide.size()) {
        const wchar_t last = wide[copyLen - 1U];
        if (last >= 0xD800 && last <= 0xDBFF) {
            --copyLen;
        }
    }
    if (copyLen > 0U) {
        std::memcpy(dst, wide.data(), copyLen * sizeof(wchar_t));
    }
    dst[copyLen] = L'\0';
    return true;
}

void AppendJsonStringUtf8(std::string& out, std::string_view utf8) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char ch : utf8) {
        switch (ch) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (ch < 0x20U) {
                    out.append("\\u00");
                    out.push_back(kHex[(ch >> 4U) & 0x0FU]);
                    out.push_back(kHex[ch & 0x0FU]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

[[nodiscard]] bool AppendJsonWideString(std::string& out, const wchar_t* ws) {
    std::string utf8;
    if (!WideToUtf8(ws, utf8)) {
        return false;
    }
    AppendJsonStringUtf8(out, utf8);
    return true;
}

void AppendIndent(std::string& out, int depth) {
    out.append(static_cast<size_t>(depth * 2), ' ');
}

void AppendUint(std::string& out, uint64_t v) {
    char buf[32]{};
    const int n = std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    if (n > 0) {
        out.append(buf, static_cast<size_t>(n));
    } else {
        out.push_back('0');
    }
}

void AppendInt(std::string& out, int64_t v) {
    char buf[32]{};
    const int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    if (n > 0) {
        out.append(buf, static_cast<size_t>(n));
    } else {
        out.push_back('0');
    }
}

void AppendBool(std::string& out, bool v) {
    out.append(v ? "true" : "false");
}

void AppendHexBytes32(std::string& out, const uint8_t bytes[32]) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (size_t i = 0; i < 32U; ++i) {
        const uint8_t b = bytes[i];
        out.push_back(kHex[(b >> 4U) & 0x0FU]);
        out.push_back(kHex[b & 0x0FU]);
    }
    out.push_back('"');
}

[[nodiscard]] int HexVal(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

[[nodiscard]] bool DecodeDeviceIdHex(std::string_view hexStr, uint8_t outBytes[32]) noexcept {
    std::memset(outBytes, 0, 32U);
    if (hexStr.empty()) {
        return true;
    }
    size_t byteIdx = 0U;
    int highNibble = -1;
    for (const char ch : hexStr) {
        if (ch == ' ' || ch == '-' || ch == ':') {
            continue;
        }
        const int v = HexVal(ch);
        if (v < 0) {
            return false;
        }
        if (highNibble < 0) {
            highNibble = v;
        } else {
            if (byteIdx < 32U) {
                outBytes[byteIdx++] = static_cast<uint8_t>((highNibble << 4) | v);
            }
            highNibble = -1;
        }
    }
    return highNibble < 0;
}

void AppendCodePointUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7FU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

class JsonReader {
public:
    explicit JsonReader(std::string_view src) noexcept : src_(src) {
        if (src_.size() >= 3U &&
            static_cast<unsigned char>(src_[0]) == 0xEFU &&
            static_cast<unsigned char>(src_[1]) == 0xBBU &&
            static_cast<unsigned char>(src_[2]) == 0xBFU) {
            pos_ = 3U;
        }
    }

    void SkipWs() noexcept {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool AtEnd() const noexcept {
        return pos_ >= src_.size();
    }

    [[nodiscard]] char Peek() noexcept {
        SkipWs();
        return (pos_ < src_.size()) ? src_[pos_] : '\0';
    }

    [[nodiscard]] bool Consume(char expected) noexcept {
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool ParseBool(bool& out) noexcept {
        SkipWs();
        if (src_.substr(pos_, 4U) == "true") {
            pos_ += 4U;
            out = true;
            return true;
        }
        if (src_.substr(pos_, 5U) == "false") {
            pos_ += 5U;
            out = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool ParseString(std::string& out) {
        out.clear();
        SkipWs();
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            return false;
        }
        ++pos_;

        while (pos_ < src_.size()) {
            const unsigned char c = static_cast<unsigned char>(src_[pos_++]);
            if (c == '"') {
                return true;
            }
            if (c < 0x20U) {
                return false; // Unescaped control characters are invalid in JSON strings
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= src_.size()) {
                return false;
            }
            const char esc = src_[pos_++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t u1 = 0U;
                    if (!ParseHex4(u1)) {
                        return false;
                    }
                    if (u1 >= 0xD800U && u1 <= 0xDBFFU) {
                        if (pos_ + 6U > src_.size() || src_[pos_] != '\\' || src_[pos_ + 1U] != 'u') {
                            return false;
                        }
                        pos_ += 2U;
                        uint32_t u2 = 0U;
                        if (!ParseHex4(u2) || u2 < 0xDC00U || u2 > 0xDFFFU) {
                            return false;
                        }
                        const uint32_t cp = 0x10000U + (((u1 - 0xD800U) << 10U) | (u2 - 0xDC00U));
                        AppendCodePointUtf8(out, cp);
                    } else if (u1 >= 0xDC00U && u1 <= 0xDFFFU) {
                        return false; // Unpaired low surrogate
                    } else {
                        AppendCodePointUtf8(out, u1);
                    }
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool ParseUint64(uint64_t& out) noexcept {
        SkipWs();
        if (pos_ >= src_.size()) {
            return false;
        }
        if (src_[pos_] == '-') {
            // Validate number syntax, reject negative for pure unsigned or let caller handle signed
            return false;
        }
        const size_t start = pos_;
        if (src_[pos_] == '0') {
            ++pos_;
            if (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                return false; // Leading zeroes invalid per RFC 8259
            }
            if (pos_ < src_.size() && (src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E')) {
                return false;
            }
            out = 0ULL;
            return true;
        }
        if (src_[pos_] < '1' || src_[pos_] > '9') {
            return false;
        }
        uint64_t acc = 0ULL;
        while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
            const uint64_t d = static_cast<uint64_t>(src_[pos_] - '0');
            if (acc > ((std::numeric_limits<uint64_t>::max)() - d) / 10ULL) {
                acc = (std::numeric_limits<uint64_t>::max)();
            } else {
                acc = acc * 10ULL + d;
            }
            ++pos_;
        }
        if (pos_ == start) {
            return false;
        }
        if (pos_ < src_.size() && (src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E')) {
            return false;
        }
        out = acc;
        return true;
    }

    [[nodiscard]] bool ParseInt64(int64_t& out) noexcept {
        SkipWs();
        if (pos_ >= src_.size()) {
            return false;
        }
        bool neg = false;
        if (src_[pos_] == '-') {
            neg = true;
            ++pos_;
        }
        uint64_t mag = 0ULL;
        if (!ParseDigitsExact(mag)) {
            return false;
        }
        if (pos_ < src_.size() && (src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E')) {
            return false;
        }
        if (neg) {
            constexpr uint64_t kMaxAbsMin = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) + 1ULL;
            if (mag >= kMaxAbsMin) {
                out = (std::numeric_limits<int64_t>::min)();
            } else {
                out = -static_cast<int64_t>(mag);
            }
        } else {
            constexpr uint64_t kMaxPos = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
            out = (mag >= kMaxPos) ? (std::numeric_limits<int64_t>::max)() : static_cast<int64_t>(mag);
        }
        return true;
    }

    [[nodiscard]] bool ParseFlexibleUint32(uint32_t& out) noexcept {
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '-') {
            int64_t sv = 0;
            if (!ParseInt64(sv)) {
                return false;
            }
            const int64_t clamped = std::clamp<int64_t>(
                sv,
                (std::numeric_limits<int32_t>::min)(),
                (std::numeric_limits<int32_t>::max)());
            out = static_cast<uint32_t>(static_cast<int32_t>(clamped));
            return true;
        }
        uint64_t uv = 0ULL;
        if (!ParseUint64(uv)) {
            return false;
        }
        out = static_cast<uint32_t>(std::min<uint64_t>(uv, (std::numeric_limits<uint32_t>::max)()));
        return true;
    }

    [[nodiscard]] bool SkipValue(size_t depth = 0U) {
        if (depth >= kMaxJsonNestingDepth) {
            return false;
        }
        const char ch = Peek();
        if (ch == '\0') {
            return false;
        }
        if (ch == '"') {
            std::string scratch;
            return ParseString(scratch);
        }
        if (ch == 't') {
            bool b = false;
            return ParseBool(b) && b;
        }
        if (ch == 'f') {
            bool b = true;
            return ParseBool(b) && !b;
        }
        if (ch == 'n') {
            if (src_.substr(pos_, 4U) == "null") {
                pos_ += 4U;
                return true;
            }
            return false;
        }
        if (ch == '{') {
            ++pos_;
            if (Consume('}')) {
                return true;
            }
            std::string key;
            for (;;) {
                if (!ParseString(key) || !Consume(':') || !SkipValue(depth + 1U)) {
                    return false;
                }
                if (Consume('}')) {
                    return true;
                }
                if (!Consume(',')) {
                    return false;
                }
            }
        }
        if (ch == '[') {
            ++pos_;
            if (Consume(']')) {
                return true;
            }
            for (;;) {
                if (!SkipValue(depth + 1U)) {
                    return false;
                }
                if (Consume(']')) {
                    return true;
                }
                if (!Consume(',')) {
                    return false;
                }
            }
        }
        return SkipNumber();
    }

    template <typename Fn>
    [[nodiscard]] bool ParseObject(Fn&& onField) {
        if (!Consume('{')) {
            return false;
        }
        if (Consume('}')) {
            return true;
        }
        std::string key;
        for (;;) {
            if (!ParseString(key) || !Consume(':')) {
                return false;
            }
            if (!onField(std::string_view(key))) {
                return false;
            }
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
        }
    }

    template <typename Fn>
    [[nodiscard]] bool ParseArray(Fn&& onElement) {
        if (!Consume('[')) {
            return false;
        }
        if (Consume(']')) {
            return true;
        }
        size_t index = 0U;
        for (;;) {
            if (!onElement(index++)) {
                return false;
            }
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
        }
    }

private:
    [[nodiscard]] bool ParseHex4(uint32_t& out) noexcept {
        if (pos_ + 4U > src_.size()) {
            return false;
        }
        uint32_t val = 0U;
        for (size_t i = 0U; i < 4U; ++i) {
            const int d = HexVal(src_[pos_++]);
            if (d < 0) {
                return false;
            }
            val = (val << 4U) | static_cast<uint32_t>(d);
        }
        out = val;
        return true;
    }

    [[nodiscard]] bool ParseDigitsExact(uint64_t& out) noexcept {
        if (pos_ >= src_.size()) {
            return false;
        }
        if (src_[pos_] == '0') {
            ++pos_;
            if (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                return false;
            }
            out = 0ULL;
            return true;
        }
        if (src_[pos_] < '1' || src_[pos_] > '9') {
            return false;
        }
        uint64_t acc = 0ULL;
        while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
            const uint64_t d = static_cast<uint64_t>(src_[pos_] - '0');
            if (acc > ((std::numeric_limits<uint64_t>::max)() - d) / 10ULL) {
                acc = (std::numeric_limits<uint64_t>::max)();
            } else {
                acc = acc * 10ULL + d;
            }
            ++pos_;
        }
        out = acc;
        return true;
    }

    [[nodiscard]] bool SkipNumber() noexcept {
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= src_.size()) {
            return false;
        }
        if (src_[pos_] == '0') {
            ++pos_;
            if (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                return false;
            }
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        } else {
            return false;
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }
        return true;
    }

    std::string_view src_;
    size_t           pos_{0U};
};

struct StagingBank {
    std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>                 macros{};
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences> sequences{};
    std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>       triggers{};
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>             remaps{};
    std::array<bool, mask::core::kMaxMacroSlots>                                    macroUsed{};
    std::array<bool, mask::core::kMaxComplexSequences>                              seqUsed{};
    std::array<bool, mask::core::kMaxColorTriggers>                                 trigUsed{};
    std::array<bool, mask::core::kMaxRemapRules>                                    remapUsed{};
};

template <size_t N>
[[nodiscard]] size_t AllocateSlot(std::array<bool, N>& used, int64_t requestedSlot) noexcept {
    if (requestedSlot >= 0) {
        const size_t clamped = static_cast<size_t>(std::clamp<int64_t>(requestedSlot, 0, static_cast<int64_t>(N - 1U)));
        if (!used[clamped]) {
            used[clamped] = true;
            return clamped;
        }
    }
    for (size_t i = 0U; i < N; ++i) {
        if (!used[i]) {
            used[i] = true;
            return i;
        }
    }
    return N;
}

[[nodiscard]] bool ParseAutoclickerObject(
    JsonReader& reader,
    mask::core::MacroConfig& out,
    int64_t& outRequestedSlot
) {
    out = mask::core::MacroConfig{};
    outRequestedSlot = -1;
    std::string strBuf;

    const bool ok = reader.ParseObject([&](std::string_view key) -> bool {
        if (key == "slotIndex") {
            return reader.ParseInt64(outRequestedSlot);
        }
        if (key == "name") {
            return reader.ParseString(strBuf) && Utf8ToWideFixed(strBuf, out.name, std::size(out.name));
        }
        if (key == "enabled") {
            return reader.ParseBool(out.enabled);
        }
        if (key == "mode") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.mode = static_cast<uint8_t>(std::clamp<int64_t>(
                v, 0, static_cast<int64_t>(mask::core::MacroMode::SocdPair)));
            return true;
        }
        if (key == "triggerVk") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.triggerVk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "outputVk") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.outputVk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "dwellDurationNs") {
            uint64_t v = 0ULL;
            if (!reader.ParseUint64(v)) return false;
            out.dwellDurationNs = v; // Unrestricted
            return true;
        }
        if (key == "recoveryDurationNs") {
            uint64_t v = 0ULL;
            if (!reader.ParseUint64(v)) return false;
            out.recoveryDurationNs = v; // Unrestricted
            return true;
        }
        if (key == "repeatCount") {
            uint64_t v = 0ULL;
            if (!reader.ParseUint64(v)) return false;
            out.repeatCount = static_cast<uint32_t>(std::min<uint64_t>(
                v, (std::numeric_limits<uint32_t>::max)()));
            return true;
        }
        if (key == "mouseDeltaX") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.mouseDeltaX = static_cast<int32_t>(std::clamp<int64_t>(
                v, (std::numeric_limits<int32_t>::min)(), (std::numeric_limits<int32_t>::max)()));
            return true;
        }
        if (key == "mouseDeltaY") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.mouseDeltaY = static_cast<int32_t>(std::clamp<int64_t>(
                v, (std::numeric_limits<int32_t>::min)(), (std::numeric_limits<int32_t>::max)()));
            return true;
        }
        if (key == "humanizePct") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            mask::core::SetHumanizePercent(out, static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 50)));
            return true;
        }
        if (key == "stepCount") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.stepCount = static_cast<uint8_t>(std::clamp<int64_t>(
                v, 1, static_cast<int64_t>(mask::core::kMaxSequenceSteps)));
            return true;
        }
        if (key == "steps") {
            uint8_t count = 0U;
            const bool arrOk = reader.ParseArray([&](size_t idx) -> bool {
                if (idx >= mask::core::kMaxSequenceSteps) {
                    return reader.SkipValue(2U);
                }
                mask::core::SequenceStepConfig st{};
                const bool stOk = reader.ParseObject([&](std::string_view sk) -> bool {
                    if (sk == "outputVk") {
                        int64_t v = 0;
                        if (!reader.ParseInt64(v)) return false;
                        st.outputVk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
                        return true;
                    }
                    if (sk == "dwellDurationNs") {
                        uint64_t v = 0ULL;
                        if (!reader.ParseUint64(v)) return false;
                        st.dwellDurationNs = v;
                        return true;
                    }
                    if (sk == "recoveryDurationNs") {
                        uint64_t v = 0ULL;
                        if (!reader.ParseUint64(v)) return false;
                        st.recoveryDurationNs = v;
                        return true;
                    }
                    if (sk == "enabled") {
                        return reader.ParseBool(st.enabled);
                    }
                    return reader.SkipValue(3U);
                });
                if (!stOk) return false;
                st.isMouse = IsMouseVk(st.outputVk);
                out.steps[idx] = st;
                count = static_cast<uint8_t>(idx + 1U);
                return true;
            });
            if (arrOk && count > 0U && out.stepCount == 0U) {
                out.stepCount = count;
            }
            return arrOk;
        }
        return reader.SkipValue(2U);
    });
    if (!ok) {
        return false;
    }

    out.isMouse = IsMouseVk(out.outputVk);
    for (auto& st : out.steps) {
        st.isMouse = IsMouseVk(st.outputVk);
    }
    out.stepCount = static_cast<uint8_t>(std::clamp<int>(
        out.stepCount, 1, static_cast<int>(mask::core::kMaxSequenceSteps)));
    out.reserved[0] = 1U;
    out.reserved[2] = 0U;
    out.name[std::size(out.name) - 1U] = L'\0';
    return true;
}

[[nodiscard]] bool ParseSequenceStepObject(JsonReader& reader, mask::core::ComplexStep& st) {
    st = mask::core::ComplexStep{};
    std::string strBuf;
    const bool ok = reader.ParseObject([&](std::string_view key) -> bool {
        if (key == "kind") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            if (v < 0 || v > static_cast<int64_t>(mask::core::ComplexStepKind::EndIf)) {
                st.kind = static_cast<uint8_t>(mask::core::ComplexStepKind::WaitMs);
            } else {
                st.kind = static_cast<uint8_t>(v);
            }
            return true;
        }
        if (key == "flags") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            st.flags = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "vk") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            st.vk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "x") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            st.x = static_cast<int32_t>(std::clamp<int64_t>(
                v, (std::numeric_limits<int32_t>::min)(), (std::numeric_limits<int32_t>::max)()));
            return true;
        }
        if (key == "y") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            st.y = static_cast<int32_t>(std::clamp<int64_t>(
                v, (std::numeric_limits<int32_t>::min)(), (std::numeric_limits<int32_t>::max)()));
            return true;
        }
        if (key == "durationMs") {
            uint64_t v = 0ULL;
            if (!reader.ParseUint64(v)) return false;
            st.durationMs = static_cast<uint32_t>(std::min<uint64_t>(
                v, (std::numeric_limits<uint32_t>::max)())); // Unrestricted
            return true;
        }
        if (key == "param") {
            return reader.ParseFlexibleUint32(st.param);
        }
        if (key == "r") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            st.r = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "g") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            st.g = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "b") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            st.b = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "tolerance") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            st.tolerance = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "text") {
            return reader.ParseString(strBuf) && Utf8ToWideFixed(strBuf, st.text, std::size(st.text));
        }
        return reader.SkipValue(3U);
    });
    if (!ok) {
        return false;
    }
    if (st.kind == static_cast<uint8_t>(mask::core::ComplexStepKind::IfCheck)) {
        st.flags = static_cast<uint8_t>(
            st.flags & (mask::core::kComplexCondMask | mask::core::kComplexFlagNegate));
        if ((st.flags & mask::core::kComplexCondMask) >
            static_cast<uint8_t>(mask::core::ComplexConditionKind::RandomChance)) {
            st.flags = static_cast<uint8_t>(st.flags & ~mask::core::kComplexCondMask);
        }
    }
    st.text[std::size(st.text) - 1U] = L'\0';
    return true;
}

[[nodiscard]] bool ParseSequenceObject(
    JsonReader& reader,
    mask::core::ComplexSequenceConfig& out,
    int64_t& outRequestedSlot,
    bool& outHasCommands
) {
    out = mask::core::ComplexSequenceConfig{};
    outRequestedSlot = -1;
    outHasCommands = false;
    std::string strBuf;

    const bool ok = reader.ParseObject([&](std::string_view key) -> bool {
        if (key == "slotIndex") {
            return reader.ParseInt64(outRequestedSlot);
        }
        if (key == "name") {
            return reader.ParseString(strBuf) && Utf8ToWideFixed(strBuf, out.name, std::size(out.name));
        }
        if (key == "enabled") {
            return reader.ParseBool(out.enabled);
        }
        if (key == "triggerMode") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.triggerMode = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 2));
            return true;
        }
        if (key == "triggerVk") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.triggerVk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "humanizePct") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.humanizePct = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 50));
            return true;
        }
        if (key == "steps") {
            uint16_t count = 0U;
            const bool arrOk = reader.ParseArray([&](size_t idx) -> bool {
                if (idx >= mask::core::kMaxComplexSteps) {
                    return reader.SkipValue(2U);
                }
                mask::core::ComplexStep st{};
                if (!ParseSequenceStepObject(reader, st)) {
                    return false;
                }
                out.steps[idx] = st;
                count = static_cast<uint16_t>(idx + 1U);
                return true;
            });
            if (!arrOk) {
                return false;
            }
            out.stepCount = count;
            return true;
        }
        return reader.SkipValue(2U);
    });
    if (!ok) {
        return false;
    }

    out.inUse = true;
    out.stepCount = std::min<uint16_t>(out.stepCount, static_cast<uint16_t>(mask::core::kMaxComplexSteps));
    out.name[std::size(out.name) - 1U] = L'\0';
    out.reserved[0] = 0U;
    out.reserved[1] = 0U;
    out.reserved[2] = 0U;

    for (uint16_t i = 0U; i < out.stepCount; ++i) {
        const uint8_t k = out.steps[i].kind;
        if (k == static_cast<uint8_t>(mask::core::ComplexStepKind::LaunchProgram) ||
            k == static_cast<uint8_t>(mask::core::ComplexStepKind::RunCommand)) {
            outHasCommands = true;
            break;
        }
    }
    mask::core::SetSequenceUntrustedImport(out, outHasCommands);
    return true;
}

[[nodiscard]] bool ParseTriggerObject(
    JsonReader& reader,
    mask::core::ColorTriggerConfig& out,
    int64_t& outRequestedSlot
) {
    out = mask::core::ColorTriggerConfig{};
    outRequestedSlot = -1;
    std::string strBuf;

    const bool ok = reader.ParseObject([&](std::string_view key) -> bool {
        if (key == "slotIndex") {
            return reader.ParseInt64(outRequestedSlot);
        }
        if (key == "name") {
            return reader.ParseString(strBuf) && Utf8ToWideFixed(strBuf, out.name, std::size(out.name));
        }
        if (key == "enabled") {
            return reader.ParseBool(out.enabled);
        }
        if (key == "mode") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.mode = static_cast<uint8_t>(std::clamp<int64_t>(
                v, 0, static_cast<int64_t>(mask::core::ColorTriggerMode::ColorChanged)));
            return true;
        }
        if (key == "matchRule") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            const auto rule = (v == static_cast<int64_t>(mask::core::ColorTriggerMatchRule::ZoneAverage))
                ? mask::core::ColorTriggerMatchRule::ZoneAverage
                : mask::core::ColorTriggerMatchRule::AnyPixel;
            mask::core::SetTriggerMatchRule(out, rule);
            return true;
        }
        if (key == "r") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.r = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "g") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.g = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "b") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.b = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "tolerance") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.tolerance = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "monitorIndex") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.monitorIndex = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 7));
            return true;
        }
        if (key == "sampleRadius") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.sampleRadius = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 7));
            return true;
        }
        if (key == "sampleX") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.sampleX = static_cast<int32_t>(std::clamp<int64_t>(
                v, (std::numeric_limits<int32_t>::min)(), (std::numeric_limits<int32_t>::max)()));
            return true;
        }
        if (key == "sampleY") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.sampleY = static_cast<int32_t>(std::clamp<int64_t>(
                v, (std::numeric_limits<int32_t>::min)(), (std::numeric_limits<int32_t>::max)()));
            return true;
        }
        if (key == "armMode") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.armMode = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 2));
            return true;
        }
        if (key == "armVk") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.armVk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "actionKind") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.actionKind = static_cast<uint8_t>(std::clamp<int64_t>(
                v, 0, static_cast<int64_t>(mask::core::ColorTriggerActionKind::RunComplexSeq)));
            return true;
        }
        if (key == "actionVk") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.actionVk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "linkedIndex") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.linkedIndex = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "delayMs") {
            uint64_t v = 0ULL;
            if (!reader.ParseUint64(v)) return false;
            out.delayMs = static_cast<uint32_t>(std::min<uint64_t>(
                v, (std::numeric_limits<uint32_t>::max)())); // Unrestricted
            return true;
        }
        if (key == "holdMs") {
            uint64_t v = 0ULL;
            if (!reader.ParseUint64(v)) return false;
            out.holdMs = static_cast<uint32_t>(std::min<uint64_t>(
                v, (std::numeric_limits<uint32_t>::max)())); // Unrestricted
            return true;
        }
        if (key == "cooldownMs") {
            uint64_t v = 0ULL;
            if (!reader.ParseUint64(v)) return false;
            out.cooldownMs = static_cast<uint32_t>(std::min<uint64_t>(
                v, (std::numeric_limits<uint32_t>::max)())); // Unrestricted
            return true;
        }
        return reader.SkipValue(2U);
    });
    if (!ok) {
        return false;
    }

    out.inUse = true;
    if (out.actionKind == static_cast<uint8_t>(mask::core::ColorTriggerActionKind::RunMacroSlot)) {
        out.linkedIndex = static_cast<uint8_t>(std::min<size_t>(out.linkedIndex, mask::core::kMaxMacroSlots - 1U));
    } else if (out.actionKind == static_cast<uint8_t>(mask::core::ColorTriggerActionKind::RunComplexSeq)) {
        out.linkedIndex = static_cast<uint8_t>(std::min<size_t>(out.linkedIndex, mask::core::kMaxComplexSequences - 1U));
    } else {
        out.linkedIndex = static_cast<uint8_t>(std::min<size_t>(out.linkedIndex, mask::core::kMaxMacroSlots - 1U));
    }
    out.reserved[1] = 0U;
    out.reserved[2] = 0U;
    out.name[std::size(out.name) - 1U] = L'\0';
    return true;
}

[[nodiscard]] bool ParseRemapObject(
    JsonReader& reader,
    mask::core::RemapRuleConfig& out,
    int64_t& outRequestedSlot
) {
    out = mask::core::RemapRuleConfig{};
    outRequestedSlot = -1;
    std::string strBuf;

    const bool ok = reader.ParseObject([&](std::string_view key) -> bool {
        if (key == "slotIndex") {
            return reader.ParseInt64(outRequestedSlot);
        }
        if (key == "name") {
            return reader.ParseString(strBuf) && Utf8ToWideFixed(strBuf, out.name, std::size(out.name));
        }
        if (key == "enabled") {
            return reader.ParseBool(out.enabled);
        }
        if (key == "suppressOriginal") {
            return reader.ParseBool(out.suppressOriginal);
        }
        if (key == "anyDevice") {
            return reader.ParseBool(out.anyDevice);
        }
        if (key == "deviceKind") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.deviceKind = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 2));
            return true;
        }
        if (key == "deviceLabel") {
            return reader.ParseString(strBuf) && Utf8ToWideFixed(strBuf, out.deviceLabel, std::size(out.deviceLabel));
        }
        if (key == "deviceIdHex") {
            return reader.ParseString(strBuf) && DecodeDeviceIdHex(strBuf, out.deviceIdBytes);
        }
        if (key == "layerIndex") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.layerIndex = static_cast<uint8_t>(std::clamp<int64_t>(
                v, 0, static_cast<int64_t>(mask::core::kMaxRemapLayers - 1U)));
            return true;
        }
        if (key == "unlockPolicy") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.unlockPolicy = static_cast<uint8_t>(std::clamp<int64_t>(
                v, 0, static_cast<int64_t>(mask::core::RemapUnlockPolicy::WhenLocked)));
            return true;
        }
        if (key == "actionKind") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.actionKind = static_cast<uint8_t>(std::clamp<int64_t>(
                v, 0, static_cast<int64_t>(mask::core::RemapLayerAction::WarpToMonitor)));
            return true;
        }
        if (key == "targetLayer") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.targetLayer = static_cast<uint8_t>(std::clamp<int64_t>(
                v, 0, static_cast<int64_t>(mask::core::kMaxRemapLayers - 1U)));
            return true;
        }
        if (key == "monitorIndex") {
            int64_t v = -1;
            if (!reader.ParseInt64(v)) return false;
            out.monitorIndex = static_cast<int8_t>(std::clamp<int64_t>(v, -1, 7));
            return true;
        }
        if (key == "sourceVk") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.sourceVk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        if (key == "targetVk") {
            int64_t v = 0;
            if (!reader.ParseInt64(v)) return false;
            out.targetVk = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 255));
            return true;
        }
        return reader.SkipValue(2U);
    });
    if (!ok) {
        return false;
    }

    out.inUse = true;
    out.reserved = 0U;
    out.deviceLabel[std::size(out.deviceLabel) - 1U] = L'\0';
    out.name[std::size(out.name) - 1U] = L'\0';
    return true;
}

[[nodiscard]] bool WriteFileAtomicUtf8(const wchar_t* path, const std::string& bytes) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    try {
        const std::wstring targetPath(path);
        const std::wstring tmpPath = targetPath + L".tmp";
        const std::wstring bakPath = targetPath + L".bak";

        const size_t slashPos = targetPath.find_last_of(L"\\/");
        if (slashPos != std::wstring::npos && slashPos > 0U) {
            const std::wstring dir = targetPath.substr(0U, slashPos);
            CreateDirectoryW(dir.c_str(), nullptr);
        }

        const HANDLE h = CreateFileW(
            tmpPath.c_str(),
            GENERIC_WRITE,
            0U,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }

        size_t totalWritten = 0U;
        bool writeOk = true;
        while (totalWritten < bytes.size()) {
            const size_t rem = bytes.size() - totalWritten;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(rem, 1U << 20U));
            DWORD written = 0U;
            if (WriteFile(h, bytes.data() + totalWritten, chunk, &written, nullptr) == FALSE ||
                written == 0U) {
                writeOk = false;
                break;
            }
            totalWritten += written;
        }
        if (writeOk) {
            writeOk = (FlushFileBuffers(h) != FALSE);
        }
        CloseHandle(h);

        if (!writeOk) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }

        if (GetFileAttributesW(targetPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
            ReplaceFileW(targetPath.c_str(), tmpPath.c_str(), bakPath.c_str(),
                         REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE) {
            DeleteFileW(bakPath.c_str());
            return true;
        }
        if (MoveFileExW(tmpPath.c_str(), targetPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
            return true;
        }

        DeleteFileW(tmpPath.c_str());
        return false;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool ReadFileUtf8(const wchar_t* path, std::string& outBytes) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    try {
        outBytes.clear();
        const HANDLE h = CreateFileW(
            path,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }

        LARGE_INTEGER sizeLi{};
        if (GetFileSizeEx(h, &sizeLi) == FALSE ||
            sizeLi.QuadPart <= 0 ||
            static_cast<uint64_t>(sizeLi.QuadPart) > kMaxJsonFileBytes) {
            CloseHandle(h);
            return false;
        }

        const size_t totalSize = static_cast<size_t>(sizeLi.QuadPart);
        outBytes.resize(totalSize);

        size_t totalRead = 0U;
        bool ok = true;
        while (totalRead < totalSize) {
            const DWORD want = static_cast<DWORD>(std::min<size_t>(totalSize - totalRead, 1U << 20U));
            DWORD got = 0U;
            if (ReadFile(h, outBytes.data() + totalRead, want, &got, nullptr) == FALSE || got == 0U) {
                ok = false;
                break;
            }
            totalRead += got;
        }
        CloseHandle(h);
        if (!ok) {
            outBytes.clear();
            return false;
        }
        return true;
    } catch (...) {
        outBytes.clear();
        return false;
    }
}

} // namespace

bool SerializeProfileBankToJson(
    const std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>& macros,
    const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& sequences,
    const std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& triggers,
    const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& remaps,
    std::string& outUtf8Json
) noexcept {
    try {
        std::string out;
        out.reserve(4096U);

        out.append("{\n");
        out.append("  \"schemaVersion\": ");
        AppendUint(out, kProfileJsonSchemaVersion);
        out.append(",\n");
        out.append("  \"app\": \"Mask\",\n");

        // 1. Autoclickers / Macros
        out.append("  \"autoclickers\": [");
        bool firstMacro = true;
        for (size_t i = 0U; i < macros.size(); ++i) {
            const auto& m = macros[i];
            const bool inUse = (m.reserved[0] != 0U) || (m.triggerVk != 0U) || m.enabled;
            if (!inUse) {
                continue;
            }
            out.append(firstMacro ? "\n" : ",\n");
            firstMacro = false;

            AppendIndent(out, 2);
            out.append("{\n");

            AppendIndent(out, 3); out.append("\"slotIndex\": ");          AppendUint(out, i); out.append(",\n");
            AppendIndent(out, 3); out.append("\"name\": ");               if (!AppendJsonWideString(out, m.name)) return false; out.append(",\n");
            AppendIndent(out, 3); out.append("\"enabled\": ");            AppendBool(out, m.enabled); out.append(",\n");
            AppendIndent(out, 3); out.append("\"mode\": ");               AppendUint(out, m.mode); out.append(",\n");
            AppendIndent(out, 3); out.append("\"triggerVk\": ");          AppendUint(out, m.triggerVk); out.append(",\n");
            AppendIndent(out, 3); out.append("\"outputVk\": ");           AppendUint(out, m.outputVk); out.append(",\n");
            AppendIndent(out, 3); out.append("\"dwellDurationNs\": ");    AppendUint(out, m.dwellDurationNs); out.append(",\n");
            AppendIndent(out, 3); out.append("\"recoveryDurationNs\": "); AppendUint(out, m.recoveryDurationNs); out.append(",\n");
            AppendIndent(out, 3); out.append("\"repeatCount\": ");        AppendUint(out, m.repeatCount); out.append(",\n");
            AppendIndent(out, 3); out.append("\"mouseDeltaX\": ");        AppendInt(out, m.mouseDeltaX); out.append(",\n");
            AppendIndent(out, 3); out.append("\"mouseDeltaY\": ");        AppendInt(out, m.mouseDeltaY); out.append(",\n");
            AppendIndent(out, 3); out.append("\"humanizePct\": ");        AppendUint(out, mask::core::HumanizePercent(m));

            if (m.mode == static_cast<uint8_t>(mask::core::MacroMode::Sequence)) {
                const size_t sc = static_cast<size_t>(std::clamp<int>(
                    m.stepCount, 1, static_cast<int>(mask::core::kMaxSequenceSteps)));
                out.append(",\n");
                AppendIndent(out, 3); out.append("\"stepCount\": "); AppendUint(out, sc); out.append(",\n");
                AppendIndent(out, 3); out.append("\"steps\": [\n");
                for (size_t s = 0U; s < sc; ++s) {
                    const auto& st = m.steps[s];
                    AppendIndent(out, 4);
                    out.append("{ \"outputVk\": ");           AppendUint(out, st.outputVk);
                    out.append(", \"dwellDurationNs\": ");    AppendUint(out, st.dwellDurationNs);
                    out.append(", \"recoveryDurationNs\": "); AppendUint(out, st.recoveryDurationNs);
                    out.append(", \"enabled\": ");            AppendBool(out, st.enabled);
                    out.append(" }");
                    out.append((s + 1U < sc) ? ",\n" : "\n");
                }
                AppendIndent(out, 3); out.append("]\n");
            } else {
                out.push_back('\n');
            }

            AppendIndent(out, 2);
            out.push_back('}');
        }
        if (!firstMacro) {
            out.push_back('\n');
            AppendIndent(out, 1);
        }
        out.append("],\n");

        // 2. Complex Sequences
        out.append("  \"sequences\": [");
        bool firstSeq = true;
        for (size_t i = 0U; i < sequences.size(); ++i) {
            const auto& q = sequences[i];
            if (!q.inUse) {
                continue;
            }
            out.append(firstSeq ? "\n" : ",\n");
            firstSeq = false;

            AppendIndent(out, 2);
            out.append("{\n");
            AppendIndent(out, 3); out.append("\"slotIndex\": ");   AppendUint(out, i); out.append(",\n");
            AppendIndent(out, 3); out.append("\"name\": ");        if (!AppendJsonWideString(out, q.name)) return false; out.append(",\n");
            AppendIndent(out, 3); out.append("\"enabled\": ");     AppendBool(out, q.enabled); out.append(",\n");
            AppendIndent(out, 3); out.append("\"triggerMode\": "); AppendUint(out, q.triggerMode); out.append(",\n");
            AppendIndent(out, 3); out.append("\"triggerVk\": ");   AppendUint(out, q.triggerVk); out.append(",\n");
            AppendIndent(out, 3); out.append("\"humanizePct\": "); AppendUint(out, q.humanizePct); out.append(",\n");
            AppendIndent(out, 3); out.append("\"steps\": [");

            const size_t sc = std::min<size_t>(q.stepCount, mask::core::kMaxComplexSteps);
            for (size_t s = 0U; s < sc; ++s) {
                const auto& st = q.steps[s];
                out.append(s == 0U ? "\n" : ",\n");
                AppendIndent(out, 4);
                out.append("{ \"kind\": ");       AppendUint(out, st.kind);
                out.append(", \"flags\": ");      AppendUint(out, st.flags);
                out.append(", \"vk\": ");         AppendUint(out, st.vk);
                out.append(", \"x\": ");          AppendInt(out, st.x);
                out.append(", \"y\": ");          AppendInt(out, st.y);
                out.append(", \"durationMs\": "); AppendUint(out, st.durationMs);
                out.append(", \"param\": ");
                if (st.kind == static_cast<uint8_t>(mask::core::ComplexStepKind::MouseScroll)) {
                    AppendInt(out, static_cast<int32_t>(st.param));
                } else {
                    AppendUint(out, st.param);
                }
                out.append(", \"r\": ");          AppendUint(out, st.r);
                out.append(", \"g\": ");          AppendUint(out, st.g);
                out.append(", \"b\": ");          AppendUint(out, st.b);
                out.append(", \"tolerance\": ");  AppendUint(out, st.tolerance);
                out.append(", \"text\": ");       if (!AppendJsonWideString(out, st.text)) return false;
                out.append(" }");
            }
            if (sc > 0U) {
                out.push_back('\n');
                AppendIndent(out, 3);
            }
            out.append("]\n");
            AppendIndent(out, 2);
            out.push_back('}');
        }
        if (!firstSeq) {
            out.push_back('\n');
            AppendIndent(out, 1);
        }
        out.append("],\n");

        // 3. Color Triggers
        out.append("  \"triggers\": [");
        bool firstTrig = true;
        for (size_t i = 0U; i < triggers.size(); ++i) {
            const auto& t = triggers[i];
            if (!t.inUse) {
                continue;
            }
            out.append(firstTrig ? "\n" : ",\n");
            firstTrig = false;

            AppendIndent(out, 2);
            out.append("{\n");
            AppendIndent(out, 3); out.append("\"slotIndex\": ");    AppendUint(out, i); out.append(",\n");
            AppendIndent(out, 3); out.append("\"name\": ");         if (!AppendJsonWideString(out, t.name)) return false; out.append(",\n");
            AppendIndent(out, 3); out.append("\"enabled\": ");      AppendBool(out, t.enabled); out.append(",\n");
            AppendIndent(out, 3); out.append("\"mode\": ");         AppendUint(out, t.mode); out.append(",\n");
            AppendIndent(out, 3); out.append("\"matchRule\": ");    AppendUint(out, static_cast<uint8_t>(mask::core::GetTriggerMatchRule(t))); out.append(",\n");
            AppendIndent(out, 3); out.append("\"r\": ");            AppendUint(out, t.r); out.append(",\n");
            AppendIndent(out, 3); out.append("\"g\": ");            AppendUint(out, t.g); out.append(",\n");
            AppendIndent(out, 3); out.append("\"b\": ");            AppendUint(out, t.b); out.append(",\n");
            AppendIndent(out, 3); out.append("\"tolerance\": ");    AppendUint(out, t.tolerance); out.append(",\n");
            AppendIndent(out, 3); out.append("\"monitorIndex\": "); AppendUint(out, t.monitorIndex); out.append(",\n");
            AppendIndent(out, 3); out.append("\"sampleRadius\": "); AppendUint(out, t.sampleRadius); out.append(",\n");
            AppendIndent(out, 3); out.append("\"sampleX\": ");      AppendInt(out, t.sampleX); out.append(",\n");
            AppendIndent(out, 3); out.append("\"sampleY\": ");      AppendInt(out, t.sampleY); out.append(",\n");
            AppendIndent(out, 3); out.append("\"armMode\": ");      AppendUint(out, t.armMode); out.append(",\n");
            AppendIndent(out, 3); out.append("\"armVk\": ");        AppendUint(out, t.armVk); out.append(",\n");
            AppendIndent(out, 3); out.append("\"actionKind\": ");   AppendUint(out, t.actionKind); out.append(",\n");
            AppendIndent(out, 3); out.append("\"actionVk\": ");     AppendUint(out, t.actionVk); out.append(",\n");
            AppendIndent(out, 3); out.append("\"linkedIndex\": ");  AppendUint(out, t.linkedIndex); out.append(",\n");
            AppendIndent(out, 3); out.append("\"delayMs\": ");      AppendUint(out, t.delayMs); out.append(",\n");
            AppendIndent(out, 3); out.append("\"holdMs\": ");       AppendUint(out, t.holdMs); out.append(",\n");
            AppendIndent(out, 3); out.append("\"cooldownMs\": ");   AppendUint(out, t.cooldownMs); out.push_back('\n');
            AppendIndent(out, 2);
            out.push_back('}');
        }
        if (!firstTrig) {
            out.push_back('\n');
            AppendIndent(out, 1);
        }
        out.append("],\n");

        // 4. Remap Rules
        out.append("  \"remaps\": [");
        bool firstRemap = true;
        for (size_t i = 0U; i < remaps.size(); ++i) {
            const auto& r = remaps[i];
            if (!r.inUse) {
                continue;
            }
            out.append(firstRemap ? "\n" : ",\n");
            firstRemap = false;

            AppendIndent(out, 2);
            out.append("{\n");
            AppendIndent(out, 3); out.append("\"slotIndex\": ");        AppendUint(out, i); out.append(",\n");
            AppendIndent(out, 3); out.append("\"name\": ");             if (!AppendJsonWideString(out, r.name)) return false; out.append(",\n");
            AppendIndent(out, 3); out.append("\"enabled\": ");          AppendBool(out, r.enabled); out.append(",\n");
            AppendIndent(out, 3); out.append("\"suppressOriginal\": "); AppendBool(out, r.suppressOriginal); out.append(",\n");
            AppendIndent(out, 3); out.append("\"anyDevice\": ");        AppendBool(out, r.anyDevice); out.append(",\n");
            AppendIndent(out, 3); out.append("\"deviceKind\": ");       AppendUint(out, r.deviceKind); out.append(",\n");
            AppendIndent(out, 3); out.append("\"deviceLabel\": ");      if (!AppendJsonWideString(out, r.deviceLabel)) return false; out.append(",\n");
            AppendIndent(out, 3); out.append("\"deviceIdHex\": ");      AppendHexBytes32(out, r.deviceIdBytes); out.append(",\n");
            AppendIndent(out, 3); out.append("\"layerIndex\": ");       AppendUint(out, r.layerIndex); out.append(",\n");
            AppendIndent(out, 3); out.append("\"unlockPolicy\": ");     AppendUint(out, r.unlockPolicy); out.append(",\n");
            AppendIndent(out, 3); out.append("\"actionKind\": ");       AppendUint(out, r.actionKind); out.append(",\n");
            AppendIndent(out, 3); out.append("\"targetLayer\": ");      AppendUint(out, r.targetLayer); out.append(",\n");
            AppendIndent(out, 3); out.append("\"monitorIndex\": ");     AppendInt(out, r.monitorIndex); out.append(",\n");
            AppendIndent(out, 3); out.append("\"sourceVk\": ");         AppendUint(out, r.sourceVk); out.append(",\n");
            AppendIndent(out, 3); out.append("\"targetVk\": ");         AppendUint(out, r.targetVk); out.push_back('\n');
            AppendIndent(out, 2);
            out.push_back('}');
        }
        if (!firstRemap) {
            out.push_back('\n');
            AppendIndent(out, 1);
        }
        out.append("]\n");
        out.append("}\n");

        outUtf8Json = std::move(out);
        return true;
    } catch (...) {
        return false;
    }
}

bool DeserializeProfileBankFromJson(
    const std::string& utf8Json,
    std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>& outMacros,
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& outSequences,
    std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& outTriggers,
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& outRemaps,
    bool& outHasImportedCommands
) noexcept {
    outHasImportedCommands = false;
    try {
        if (utf8Json.empty()) {
            return false;
        }

        auto staging = std::make_unique<StagingBank>();
        for (size_t i = 0U; i < staging->macros.size(); ++i) {
            staging->macros[i].slotIndex = static_cast<uint8_t>(i);
        }
        for (size_t i = 0U; i < staging->sequences.size(); ++i) {
            staging->sequences[i].slotIndex = static_cast<uint8_t>(i);
        }
        for (size_t i = 0U; i < staging->triggers.size(); ++i) {
            staging->triggers[i].slotIndex = static_cast<uint8_t>(i);
        }
        for (size_t i = 0U; i < staging->remaps.size(); ++i) {
            staging->remaps[i].slotIndex = static_cast<uint8_t>(i);
        }

        JsonReader reader(utf8Json);
        bool sawKnownRootKey = false;
        bool hasImportedCommands = false;
        std::string strBuf;

        const bool parsedRoot = reader.ParseObject([&](std::string_view key) -> bool {
            if (key == "schemaVersion") {
                sawKnownRootKey = true;
                int64_t ver = 0;
                if (!reader.ParseInt64(ver)) {
                    return false;
                }
                return ver >= 1 && ver <= static_cast<int64_t>(kProfileJsonSchemaVersion);
            }
            if (key == "app") {
                sawKnownRootKey = true;
                if (!reader.ParseString(strBuf)) {
                    return false;
                }
                return strBuf == "Mask";
            }
            if (key == "autoclickers") {
                sawKnownRootKey = true;
                return reader.ParseArray([&](size_t) -> bool {
                    mask::core::MacroConfig item{};
                    int64_t reqSlot = -1;
                    if (!ParseAutoclickerObject(reader, item, reqSlot)) {
                        return false;
                    }
                    const size_t dst = AllocateSlot(staging->macroUsed, reqSlot);
                    if (dst < staging->macros.size()) {
                        item.slotIndex = static_cast<uint8_t>(dst);
                        staging->macros[dst] = item;
                    }
                    return true;
                });
            }
            if (key == "sequences") {
                sawKnownRootKey = true;
                return reader.ParseArray([&](size_t) -> bool {
                    mask::core::ComplexSequenceConfig item{};
                    int64_t reqSlot = -1;
                    bool seqHasCmd = false;
                    if (!ParseSequenceObject(reader, item, reqSlot, seqHasCmd)) {
                        return false;
                    }
                    if (seqHasCmd) {
                        hasImportedCommands = true;
                    }
                    const size_t dst = AllocateSlot(staging->seqUsed, reqSlot);
                    if (dst < staging->sequences.size()) {
                        item.slotIndex = static_cast<uint8_t>(dst);
                        staging->sequences[dst] = item;
                    }
                    return true;
                });
            }
            if (key == "triggers") {
                sawKnownRootKey = true;
                return reader.ParseArray([&](size_t) -> bool {
                    mask::core::ColorTriggerConfig item{};
                    int64_t reqSlot = -1;
                    if (!ParseTriggerObject(reader, item, reqSlot)) {
                        return false;
                    }
                    const size_t dst = AllocateSlot(staging->trigUsed, reqSlot);
                    if (dst < staging->triggers.size()) {
                        item.slotIndex = static_cast<uint8_t>(dst);
                        staging->triggers[dst] = item;
                    }
                    return true;
                });
            }
            if (key == "remaps") {
                sawKnownRootKey = true;
                return reader.ParseArray([&](size_t) -> bool {
                    mask::core::RemapRuleConfig item{};
                    int64_t reqSlot = -1;
                    if (!ParseRemapObject(reader, item, reqSlot)) {
                        return false;
                    }
                    const size_t dst = AllocateSlot(staging->remapUsed, reqSlot);
                    if (dst < staging->remaps.size()) {
                        item.slotIndex = static_cast<uint8_t>(dst);
                        staging->remaps[dst] = item;
                    }
                    return true;
                });
            }
            return reader.SkipValue(1U);
        });

        reader.SkipWs();
        if (!parsedRoot || !reader.AtEnd() || !sawKnownRootKey) {
            return false;
        }

        outMacros    = staging->macros;
        outSequences = staging->sequences;
        outTriggers  = staging->triggers;
        outRemaps    = staging->remaps;
        outHasImportedCommands = hasImportedCommands;
        return true;
    } catch (...) {
        return false;
    }
}

bool ExportProfileBankToJsonFile(
    const wchar_t* path,
    const std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>& macros,
    const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& sequences,
    const std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& triggers,
    const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& remaps
) noexcept {
    try {
        std::string utf8;
        if (!SerializeProfileBankToJson(macros, sequences, triggers, remaps, utf8)) {
            return false;
        }
        return WriteFileAtomicUtf8(path, utf8);
    } catch (...) {
        return false;
    }
}

bool ImportProfileBankFromJsonFile(
    const wchar_t* path,
    std::array<mask::core::MacroConfig, mask::core::kMaxMacroSlots>& outMacros,
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& outSequences,
    std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& outTriggers,
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& outRemaps,
    bool& outHasImportedCommands
) noexcept {
    outHasImportedCommands = false;
    try {
        std::string utf8;
        if (!ReadFileUtf8(path, utf8)) {
            return false;
        }
        return DeserializeProfileBankFromJson(
            utf8, outMacros, outSequences, outTriggers, outRemaps, outHasImportedCommands);
    } catch (...) {
        return false;
    }
}

bool RunProfileJsonSelfTest() noexcept {
    try {
        using namespace mask::core;

        auto srcMacros    = std::make_unique<std::array<MacroConfig, kMaxMacroSlots>>();
        auto srcSeqs      = std::make_unique<std::array<ComplexSequenceConfig, kMaxComplexSequences>>();
        auto srcTrigs     = std::make_unique<std::array<ColorTriggerConfig, kMaxColorTriggers>>();
        auto srcRemaps    = std::make_unique<std::array<RemapRuleConfig, kMaxRemapRules>>();

        // 1. Autoclicker with unrestricted 0 ns / sub-us timings + negative mouse deltas
        {
            auto& m = (*srcMacros)[2];
            m.slotIndex = 2U;
            m.enabled = true;
            m.mode = static_cast<uint8_t>(MacroMode::RepeatNTimes);
            m.triggerVk = VK_XBUTTON1;
            m.outputVk = VK_LBUTTON;
            m.isMouse = true;
            m.dwellDurationNs = 0ULL;
            m.recoveryDurationNs = 250ULL;
            m.repeatCount = 25000U;
            m.mouseDeltaX = -12;
            m.mouseDeltaY = 48;
            m.reserved[0] = 1U;
            SetHumanizePercent(m, 15U);
            wcscpy_s(m.name, L"Zero-ms \u6771\u4EAC");
        }

        // 2. Complex sequences: one safe, one with escaped Unicode RunCommand + LaunchProgram
        {
            auto& sSafe = (*srcSeqs)[0];
            sSafe.inUse = true;
            sSafe.enabled = true;
            sSafe.triggerMode = 1U;
            sSafe.triggerVk = VK_F7;
            sSafe.humanizePct = 5U;
            wcscpy_s(sSafe.name, L"Safe Flow");
            sSafe.stepCount = 2U;
            sSafe.steps[0].kind = static_cast<uint8_t>(ComplexStepKind::MouseScroll);
            sSafe.steps[0].param = static_cast<uint32_t>(-3);
            sSafe.steps[1].kind = static_cast<uint8_t>(ComplexStepKind::WaitMs);
            sSafe.steps[1].durationMs = 0U;
            SetSequenceUntrustedImport(sSafe, false);

            auto& sCmd = (*srcSeqs)[1];
            sCmd.inUse = true;
            sCmd.enabled = false;
            sCmd.triggerMode = 2U;
            sCmd.triggerVk = VK_F9;
            wcscpy_s(sCmd.name, L"Cmd \"Quoted\" \\ Test");
            sCmd.stepCount = 3U;
            sCmd.steps[0].kind = static_cast<uint8_t>(ComplexStepKind::LaunchProgram);
            wcscpy_s(sCmd.steps[0].text, L"C:\\Tools\\App \"v1\"\\\u00E9.exe");
            sCmd.steps[1].kind = static_cast<uint8_t>(ComplexStepKind::RunCommand);
            wcscpy_s(sCmd.steps[1].text, L"echo \"hi\\there\"\tline2\n\u65E5\u672C\u8A9E");
            sCmd.steps[2].kind = static_cast<uint8_t>(ComplexStepKind::IfCheck);
            SetStepConditionKind(sCmd.steps[2], ComplexConditionKind::ColorMatches);
            SetStepConditionNegated(sCmd.steps[2], true);
            // Explicitly clear flag on source to prove Deserialize sets it on import!
            SetSequenceUntrustedImport(sCmd, false);
        }

        // 3. Color trigger with ZoneAverage and unrestricted timing
        {
            auto& t = (*srcTrigs)[1];
            t.inUse = true;
            t.enabled = true;
            t.mode = static_cast<uint8_t>(ColorTriggerMode::ColorChanged);
            SetTriggerMatchRule(t, ColorTriggerMatchRule::ZoneAverage);
            t.r = 12U;
            t.g = 210U;
            t.b = 245U;
            t.tolerance = 32U;
            t.monitorIndex = 1U;
            t.sampleRadius = 5U;
            t.sampleX = 640;
            t.sampleY = 360;
            t.armMode = 1U;
            t.armVk = VK_RBUTTON;
            t.actionKind = static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot);
            t.actionVk = VK_LBUTTON;
            t.linkedIndex = 2U;
            t.delayMs = 0U;
            t.holdMs = 125000U;
            t.cooldownMs = 0U;
            wcscpy_s(t.name, L"Zone Trigger \u2605");
        }

        // 4. Remap rule with full 32-byte device ID and Unicode label
        {
            auto& r = (*srcRemaps)[3];
            r.inUse = true;
            r.enabled = true;
            r.suppressOriginal = true;
            r.anyDevice = false;
            r.deviceKind = 1U;
            r.layerIndex = 2U;
            r.unlockPolicy = static_cast<uint8_t>(RemapUnlockPolicy::WhenUnlocked);
            r.actionKind = static_cast<uint8_t>(RemapLayerAction::WarpToMonitor);
            r.targetLayer = 3U;
            r.monitorIndex = -1;
            r.sourceVk = 'Q';
            r.targetVk = 'E';
            for (size_t b = 0U; b < 32U; ++b) {
                r.deviceIdBytes[b] = static_cast<uint8_t>(0xA0U + b);
            }
            wcscpy_s(r.deviceLabel, L"Keychron \u00DCber Q6");
            wcscpy_s(r.name, L"Layer Warp");
        }

        std::string json;
        if (!SerializeProfileBankToJson(*srcMacros, *srcSeqs, *srcTrigs, *srcRemaps, json)) {
            return false;
        }

        auto dstMacros = std::make_unique<std::array<MacroConfig, kMaxMacroSlots>>();
        auto dstSeqs   = std::make_unique<std::array<ComplexSequenceConfig, kMaxComplexSequences>>();
        auto dstTrigs  = std::make_unique<std::array<ColorTriggerConfig, kMaxColorTriggers>>();
        auto dstRemaps = std::make_unique<std::array<RemapRuleConfig, kMaxRemapRules>>();
        bool hasImportedCommands = false;

        if (!DeserializeProfileBankFromJson(
                json, *dstMacros, *dstSeqs, *dstTrigs, *dstRemaps, hasImportedCommands)) {
            return false;
        }
        if (!hasImportedCommands) {
            return false;
        }

        // Verify Autoclicker roundtrip
        const auto& dm = (*dstMacros)[2];
        if (!dm.enabled || dm.mode != static_cast<uint8_t>(MacroMode::RepeatNTimes) ||
            dm.triggerVk != VK_XBUTTON1 || dm.outputVk != VK_LBUTTON || !dm.isMouse ||
            dm.dwellDurationNs != 0ULL || dm.recoveryDurationNs != 250ULL ||
            dm.repeatCount != 25000U || dm.mouseDeltaX != -12 || dm.mouseDeltaY != 48 ||
            HumanizePercent(dm) != 15U ||
            std::wcscmp(dm.name, L"Zero-ms \u6771\u4EAC") != 0) {
            return false;
        }

        // Verify Complex Sequence roundtrip & untrusted import tagging
        const auto& dsSafe = (*dstSeqs)[0];
        const auto& dsCmd  = (*dstSeqs)[1];
        if (!dsSafe.inUse || !dsSafe.enabled || IsSequenceUntrustedImport(dsSafe) ||
            static_cast<int32_t>(dsSafe.steps[0].param) != -3 || dsSafe.steps[1].durationMs != 0U) {
            return false;
        }
        if (!dsCmd.inUse || !IsSequenceUntrustedImport(dsCmd) || dsCmd.stepCount != 3U ||
            std::wcscmp(dsCmd.name, L"Cmd \"Quoted\" \\ Test") != 0 ||
            std::wcscmp(dsCmd.steps[0].text, L"C:\\Tools\\App \"v1\"\\\u00E9.exe") != 0 ||
            std::wcscmp(dsCmd.steps[1].text, L"echo \"hi\\there\"\tline2\n\u65E5\u672C\u8A9E") != 0 ||
            GetStepConditionKind(dsCmd.steps[2]) != ComplexConditionKind::ColorMatches ||
            !IsStepConditionNegated(dsCmd.steps[2])) {
            return false;
        }

        // Verify Color Trigger roundtrip
        const auto& dt = (*dstTrigs)[1];
        if (!dt.inUse || !dt.enabled || dt.mode != static_cast<uint8_t>(ColorTriggerMode::ColorChanged) ||
            GetTriggerMatchRule(dt) != ColorTriggerMatchRule::ZoneAverage ||
            dt.r != 12U || dt.g != 210U || dt.b != 245U || dt.tolerance != 32U ||
            dt.monitorIndex != 1U || dt.sampleRadius != 5U || dt.sampleX != 640 || dt.sampleY != 360 ||
            dt.armMode != 1U || dt.armVk != VK_RBUTTON ||
            dt.actionKind != static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot) ||
            dt.linkedIndex != 2U || dt.delayMs != 0U || dt.holdMs != 125000U || dt.cooldownMs != 0U ||
            std::wcscmp(dt.name, L"Zone Trigger \u2605") != 0) {
            return false;
        }

        // Verify Remap roundtrip
        const auto& dr = (*dstRemaps)[3];
        if (!dr.inUse || !dr.enabled || !dr.suppressOriginal || dr.anyDevice || dr.deviceKind != 1U ||
            dr.layerIndex != 2U || dr.targetLayer != 3U || dr.monitorIndex != -1 ||
            dr.sourceVk != 'Q' || dr.targetVk != 'E' ||
            std::wcscmp(dr.deviceLabel, L"Keychron \u00DCber Q6") != 0 ||
            std::memcmp(dr.deviceIdBytes, (*srcRemaps)[3].deviceIdBytes, 32U) != 0) {
            return false;
        }

        // 5. Missing optional fields, \uXXXX + surrogate pair decode, and out-of-range index clamping
        const std::string edgeJson = R"json({
          "schemaVersion": 2,
          "app": "Mask",
          "autoclickers": [
            { "name": "\u0041\u00E9\u6771\uD83D\uDE80", "slotIndex": 999, "dwellDurationNs": 0, "recoveryDurationNs": 0 }
          ],
          "sequences": [
            { "name": "SafeOnly", "steps": [ { "kind": 6, "durationMs": 0 } ] }
          ],
          "triggers": [
            { "monitorIndex": 99, "sampleRadius": 42, "actionKind": 2, "linkedIndex": 500 }
          ],
          "remaps": [
            { "layerIndex": 99, "targetLayer": 77, "monitorIndex": 99 }
          ]
        })json";

        bool edgeHasCmd = true;
        if (!DeserializeProfileBankFromJson(
                edgeJson, *dstMacros, *dstSeqs, *dstTrigs, *dstRemaps, edgeHasCmd)) {
            return false;
        }
        if (edgeHasCmd) {
            return false;
        }
        if (std::wcscmp((*dstMacros)[kMaxMacroSlots - 1U].name, L"A\u00E9\u6771\U0001F680") != 0 ||
            (*dstMacros)[kMaxMacroSlots - 1U].dwellDurationNs != 0ULL ||
            (*dstMacros)[kMaxMacroSlots - 1U].recoveryDurationNs != 0ULL ||
            IsSequenceUntrustedImport((*dstSeqs)[0]) ||
            (*dstTrigs)[0].monitorIndex != 7U || (*dstTrigs)[0].sampleRadius != 7U ||
            (*dstTrigs)[0].linkedIndex != static_cast<uint8_t>(kMaxMacroSlots - 1U) ||
            (*dstRemaps)[0].layerIndex != static_cast<uint8_t>(kMaxRemapLayers - 1U) ||
            (*dstRemaps)[0].targetLayer != static_cast<uint8_t>(kMaxRemapLayers - 1U) ||
            (*dstRemaps)[0].monitorIndex != 7) {
            return false;
        }

        // 6. Invalid JSON rejection must fail and leave existing outputs untouched
        const std::string invalidCases[] = {
            "",
            "{bad",
            "[]",
            "{\"schemaVersion\": 99, \"app\": \"Mask\"}",
            "{\"schemaVersion\": 2, \"app\": \"Other\"}",
            "{\"schemaVersion\": 2, \"app\": \"Mask\", \"autoclickers\": 123}",
            "{\"schemaVersion\": 2, \"app\": \"Mask\", \"sequences\": [{\"steps\":[{\"text\":\"\\q\"}]}]}",
            "{\"schemaVersion\": 2, \"app\": \"Mask\", \"sequences\": [{\"steps\":[{\"text\":\"\\uD800\"}]}]}",
            "{\"schemaVersion\": 2, \"app\": \"Mask\"} trailing"
        };
        for (const auto& bad : invalidCases) {
            bool dummyCmd = true;
            if (DeserializeProfileBankFromJson(
                    bad, *dstMacros, *dstSeqs, *dstTrigs, *dstRemaps, dummyCmd)) {
                return false;
            }
            if ((*dstMacros)[kMaxMacroSlots - 1U].slotIndex != static_cast<uint8_t>(kMaxMacroSlots - 1U) ||
                !(*dstTrigs)[0].inUse) {
                return false;
            }
        }

        // 7. File export/import roundtrip in %TEMP%
        wchar_t tempDir[MAX_PATH]{};
        const DWORD tdLen = GetTempPathW(MAX_PATH, tempDir);
        if (tdLen > 0U && tdLen < MAX_PATH - 40U) {
            wchar_t tempFile[MAX_PATH]{};
            std::swprintf(tempFile, std::size(tempFile), L"%lsmask_profile_json_selftest_%lu.json",
                          tempDir, static_cast<unsigned long>(GetCurrentProcessId()));
            const bool expOk = ExportProfileBankToJsonFile(
                tempFile, *srcMacros, *srcSeqs, *srcTrigs, *srcRemaps);
            bool fileHasCmd = false;
            const bool impOk = expOk && ImportProfileBankFromJsonFile(
                tempFile, *dstMacros, *dstSeqs, *dstTrigs, *dstRemaps, fileHasCmd);
            DeleteFileW(tempFile);
            if (!expOk || !impOk || !fileHasCmd || !IsSequenceUntrustedImport((*dstSeqs)[1])) {
                return false;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

} // namespace mask::profile_json
