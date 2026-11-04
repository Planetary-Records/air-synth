#include "StrictJson.h"
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

namespace airack::ai
{
namespace
{
using juce::String;
// JUCE replaces duplicate object keys. Require complete strict JSON and reject
// duplicates before materializing any auth, schema, or streaming object.
class JsonCheck
{
public:
    JsonCheck(std::string_view s, const JsonNumberObserver& observer) : text(s), observe(observer) {}
    bool valid() { return value(0) && (space(), pos == text.size()); }
    bool materialiseNumbers(juce::var& candidate)
    {
        std::size_t next = 0;
        return restoreNumbers(candidate, next) && next == numbers.size();
    }
private:
    std::string_view text; size_t pos {};
    const JsonNumberObserver& observe;
    std::vector<String> path;
    std::vector<double> numbers;
    // JUCE preserves array/property insertion order and numeric token types.
    // Replace only decimal/exponent values with the scanner's precise native
    // conversion; keep its integer/int64 and string materialization unchanged.
    bool restoreNumbers(juce::var& v, std::size_t& next) const
    {
        if (v.isDouble())
        {
            if (next == numbers.size()) return false;
            v = numbers[next++];
        }
        else if (auto* array = v.getArray())
        {
            for (auto& item : *array) if (!restoreNumbers(item, next)) return false;
        }
        else if (auto* object = v.getDynamicObject())
        {
            auto& properties = object->getProperties();
            for (int i = 0; i < properties.size(); ++i)
                if (!restoreNumbers(*properties.getVarPointerAt(i), next)) return false;
        }
        return true;
    }
    void space() { while (pos < text.size() && (text[pos]==' ' || text[pos]=='\t' || text[pos]=='\n' || text[pos]=='\r')) ++pos; }
    bool take(char c) { space(); if (pos < text.size() && text[pos]==c) { ++pos; return true; } return false; }
    bool quoted(String* decoded = nullptr)
    {
        space(); auto start = pos;
        const auto* bytes = text.data(); const auto length = text.size();
        if (pos == length || bytes[pos++] != '"') return false;
        while (pos < length)
        {
            auto c = bytes[pos++];
            if (c == '"')
            {
                if (decoded) { auto key = String::fromUTF8(bytes+start, static_cast<int>(pos-start));
                    *decoded = juce::JSON::parse("{\"k\":"+key+"}")["k"].toString(); }
                return true;
            }
            if (static_cast<unsigned char>(c) < 32) return false;
            if (c == '\\')
            {
                if (pos == length) return false; c = bytes[pos++];
                if (c == 'u') { for (int i=0;i<4;++i) if (pos==length || !std::isxdigit(static_cast<unsigned char>(bytes[pos++]))) return false; }
                else if (std::string_view("\"\\/bfnrt").find(c)==std::string_view::npos) return false;
            }
        }
        return false;
    }
    bool value(int depth)
    {
        space(); if (depth > 24 || pos==text.size()) return false;
        if (text[pos]=='"') return quoted();
        if (take('{'))
        {
            std::set<String> keys; if (take('}')) return true;
            do {
                String key; if (!quoted(&key) || !keys.insert(key).second || !take(':')) return false;
                if (observe) path.push_back(key);
                const bool valid = value(depth+1);
                if (observe) path.pop_back();
                if (!valid) return false;
            } while (take(','));
            return take('}');
        }
        if (take('[')) {
            if (take(']')) return true;
            do {
                if (observe) path.emplace_back();
                const bool valid = value(depth+1);
                if (observe) path.pop_back();
                if (!valid) return false;
            } while (take(','));
            return take(']');
        }
        for (auto literal : { "true", "false", "null" })
            if (text.substr(pos, std::strlen(literal))==literal) { pos+=std::strlen(literal); return true; }
        const auto numberStart = pos;
        if (text[pos]=='-') ++pos; if (pos==text.size()) return false;
        if (text[pos]=='0') ++pos;
        else { if (text[pos]<'1'||text[pos]>'9') return false; while (pos<text.size() && text[pos]>='0'&&text[pos]<='9') ++pos; }
        // JUCE accumulates the integer prefix before checking for decimal/exponent.
        // Reject overflow there rather than letting a large token wrap into a valid edit.
        std::int64_t integerPrefix {};
        if (std::from_chars(text.data()+numberStart, text.data()+pos, integerPrefix).ec != std::errc{}
            || integerPrefix == std::numeric_limits<std::int64_t>::min()) return false;
        const bool floatingToken = pos < text.size() && (text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E');
        if (pos<text.size() && text[pos]=='.') { ++pos; auto start=pos; while(pos<text.size()&&text[pos]>='0'&&text[pos]<='9') ++pos; if(start==pos) return false; }
        if (pos<text.size() && (text[pos]=='e'||text[pos]=='E')) { ++pos; if(pos<text.size()&&(text[pos]=='+'||text[pos]=='-')) ++pos; auto start=pos; while(pos<text.size()&&text[pos]>='0'&&text[pos]<='9') ++pos; if(start==pos) return false; }
        if (observe) observe(path, text.substr(numberStart, pos-numberStart));
        double number {};
        const auto parsed = std::from_chars(text.data()+numberStart, text.data()+pos, number);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data()+pos || !std::isfinite(number)) return false;
        if (floatingToken) numbers.push_back(number);
        return true;
    }
};

}

bool parseStrictJson(std::string_view utf8, juce::var& output, std::size_t maximumBytes)
{
    return parseStrictJson(utf8, output, maximumBytes, {});
}

bool parseStrictJson(std::string_view utf8, juce::var& output, std::size_t maximumBytes,
                     const JsonNumberObserver& observe)
{
    if (utf8.empty() || utf8.size() > maximumBytes || utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || utf8.find('\0') != std::string_view::npos
        || !juce::CharPointer_UTF8::isValidString(utf8.data(), static_cast<int>(utf8.size()))) return false;
    JsonCheck check(utf8, observe);
    if (!check.valid()) return false;
    juce::var candidate;
    if (juce::JSON::parse(String::fromUTF8(utf8.data(), static_cast<int>(utf8.size())), candidate).failed()) return false;
    if (!check.materialiseNumbers(candidate)) return false;
    output = candidate;
    return true;
}
}
