#pragma once

namespace airack::space::detail {
// Off-audio preparation only. The caller owns context until preparation returns.
struct PreparationCancelled {};
struct PreparationControl {
    bool (*isCancelled)(void*) noexcept=nullptr;
    void* context=nullptr;
    void checkpoint() const {
        if(isCancelled&&isCancelled(context))throw PreparationCancelled{};
    }
};
}
