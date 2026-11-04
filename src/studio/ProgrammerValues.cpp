#include "ProgrammerValues.h"

#include "ParameterRegistry.h"

#include <cmath>

namespace airack::studio
{
namespace
{
using juce::String;

/** The prototype's threeFigures: no decimals from 99.95, one from 9.995, else two. A value that rounds to zero
    reads without a sign. */
String threeFigures (double v)
{
    const auto a = std::abs (v);
    // juce::String (v, 0) would fall back to the default format, decimals and all.
    auto text = a >= 99.95 ? String ((juce::int64) std::llround (v)) : String (v, a >= 9.995 ? 1 : 2);
    if (text.startsWithChar ('-') && text.removeCharacters ("-0.").isEmpty())
        text = text.substring (1);
    return text;
}

String signedFigures (double v) { return (v >= 0.0005 ? "+" : "") + threeFigures (v); }

/** What the display shows: the number in `unit`, with the factor from the registry's unit to it. */
struct Shown
{
    String unit;
    double factor = 1.0;
    bool sign = false;
};

Shown shownFor (const ParameterDescriptor& d, double phys)
{
    if(d.id.contains(".saturation.")&&d.id.endsWith(".mix"))return { "%", 100.0 };
    if(d.id.endsWith(".spectral_density"))return {"%",100.};
    const String unit (d.unit);
    if (unit == "Hz") return phys >= 999.5 ? Shown { "kHz", 0.001 } : Shown { "Hz" };
    if (unit == "s") return phys < 0.9995 ? Shown { "ms", 1000.0 } : Shown { "s" };
    if (unit == "ms") return phys >= 999.5 ? Shown { "s", 0.001 } : Shown { "ms" };
    if (unit == "dB" || unit == "st" || unit == "cent" || unit == "oct") return { unit, 1.0, true };
    if (unit.isNotEmpty()) return { unit };
    return { {}, 1.0, d.minimum < 0.0f };
}

/** The unit a typed value's prefix scales, and the registry unit's size in it. */
struct Base
{
    String unit;
    double perRegistryUnit = 1.0;
};

Base baseFor (const ParameterDescriptor& d)
{
    if(d.id.endsWith(".spectral_density"))return {"%",100.};
    const String unit (d.unit);
    if (unit == "ms") return { "s", 0.001 };
    return { unit.toLowerCase(), 1.0 };
}

/** A decimal number at the start of `text`: digits with an optional point and exponent, signed. */
std::optional<double> leadingNumber (const String& text, int& end)
{
    const auto* s = text.toRawUTF8();
    int i = 0;
    if (s[i] == '+' || s[i] == '-')
        ++i;
    const auto digitsFrom = i;
    while (juce::CharacterFunctions::isDigit (s[i]))
        ++i;
    auto digits = i - digitsFrom;
    if (s[i] == '.')
    {
        ++i;
        const auto fraction = i;
        while (juce::CharacterFunctions::isDigit (s[i]))
            ++i;
        digits += i - fraction;
    }
    if (digits == 0)
        return {};
    if (s[i] == 'e' || s[i] == 'E')
    {
        auto j = i + 1;
        if (s[j] == '+' || s[j] == '-')
            ++j;
        if (juce::CharacterFunctions::isDigit (s[j]))
        {
            while (juce::CharacterFunctions::isDigit (s[j]))
                ++j;
            i = j;
        }
    }
    end = i;
    return std::strtod (String (s, (size_t) i).toRawUTF8(), nullptr);
}
} // namespace

juce::String formatParameter (std::size_t index, float physical)
{
    const auto& d = parameterRegistry()[index];
    if (d.kind != ParameterKind::continuous)
        return String (d.choices[(size_t) juce::jlimit (0, (int) d.choices.size() - 1, (int) std::lround (physical))]).toUpperCase();
    const auto shown = shownFor (d, physical);
    const auto v = (double) physical * shown.factor;
    const auto number = shown.sign ? signedFigures (v) : threeFigures (v);
    return shown.unit.isEmpty() ? number : number + " " + shown.unit;
}

juce::String unitShown (std::size_t index, float physical)
{
    const auto& d = parameterRegistry()[index];
    return d.kind == ParameterKind::continuous ? shownFor (d, physical).unit : String();
}

std::optional<float> parseParameter (std::size_t index, const juce::String& text, const juce::String& shownUnit)
{
    const auto& d = parameterRegistry()[index];
    const auto typed = text.trim();
    if (typed.isEmpty())
        return {};
    if (d.kind != ParameterKind::continuous)
    {
        for (size_t i = 0; i < d.choices.size(); ++i)
            if (typed.equalsIgnoreCase (d.choices[i]))
                return (float) i;
        return {};
    }
    int end = 0;
    const auto number = leadingNumber (typed, end);
    if (! number || ! std::isfinite (*number))
        return {};
    const auto rest = typed.substring (end).trim().toLowerCase();
    const auto base = baseFor (d);
    const auto prefixed = base.unit == "hz" || base.unit == "s";
    double inBase = *number;
    if(d.id.contains(".saturation.")&&d.id.endsWith(".mix")&&(rest=="%"||(rest.isEmpty()&&shownUnit=="%")))
        inBase *= .01;
    else if (rest.isEmpty())
    {
        if (shownUnit == "kHz") inBase *= 1000.0;
        else if (shownUnit == "ms") inBase *= 0.001;
    }
    else if (base.unit.isNotEmpty() && rest == base.unit)
    {
    }
    else if (prefixed && (rest == "k" || rest == "m" || rest == "k" + base.unit || rest == "m" + base.unit))
        inBase *= rest.startsWithChar ('k') ? 1000.0 : 0.001;
    else
        return {};
    const auto physical = inBase / base.perRegistryUnit;
    if (! std::isfinite (physical))
        return {};
    if (physical >= d.minimum && physical <= d.maximum)
        return (float) physical;
    const auto bound = physical < d.minimum ? d.minimum : d.maximum;
    if (formatParameter (index, (float) physical) == formatParameter (index, bound))
        return bound;
    return {};
}
} // namespace airack::studio
