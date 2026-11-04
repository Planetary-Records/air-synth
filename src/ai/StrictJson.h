#pragma once
#include <juce_core/juce_core.h>
#include <cstddef>
#include <string_view>
#include <functional>
#include <span>

namespace airack::ai
{
bool parseStrictJson(std::string_view utf8, juce::var& output,
                     std::size_t maximumBytes = 4 * 1024 * 1024);
// Numeric tokens are borrowed before conversion; array steps have an empty path component.
using JsonNumberObserver = std::function<void(std::span<const juce::String>, std::string_view)>;
bool parseStrictJson(std::string_view utf8, juce::var& output,
                     std::size_t maximumBytes, const JsonNumberObserver&);
}
