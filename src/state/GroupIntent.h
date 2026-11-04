#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace airack
{
inline constexpr std::size_t maximumGroupTextBytes = 128;
struct GroupText
{
    std::array<char, maximumGroupTextBytes> bytes {};
    std::uint16_t length = 0;
    bool operator==(const GroupText&) const = default;
};
bool setGroupText(std::string_view, GroupText&) noexcept;
std::string_view groupTextView(const GroupText&) noexcept;
struct GroupUuid
{
    std::array<std::uint8_t, 16> bytes {};
    GroupUuid() = default;
    GroupUuid(const juce::Uuid&) noexcept;
    GroupUuid& operator=(const juce::Uuid&) noexcept;
    bool isNull() const noexcept;
    const std::uint8_t* getRawData() const noexcept;
    bool operator==(const GroupUuid&) const = default;
};
struct GroupIntent
{
    GroupUuid group {}, member {};
    GroupText name {}, role {}, key {}, scale {};
    std::optional<double> bpm;
    bool operator==(const GroupIntent&) const = default;
};
bool validateGroupIntent(const GroupIntent&) noexcept;
}
