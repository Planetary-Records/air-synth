#pragma once
#include <memory>

namespace airack::testing
{
// Build a large fixture directly in owned storage, including a by-value return.
// Keep the normal Windows stack available for the production call under test.
template<class F> auto fixtureValue(F&& create)
{
    using T=decltype(create());
    return std::unique_ptr<T>(new T(create()));
}
}
