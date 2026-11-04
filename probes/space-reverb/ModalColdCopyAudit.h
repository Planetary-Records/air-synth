#pragma once
#include <cstddef>
#if defined(AIRACK_MODAL_COLD_COPY_DIAGNOSTIC)
namespace airack::space::probe {
struct ModalColdCopyAccounting {
    std::size_t fullCoefficientValuesWritten=0,factoryCoefficientValuesWritten=0,stateValuesCleared=0;
};
inline thread_local ModalColdCopyAccounting modalColdCopyAccounting;
inline void clearModalColdCopyAccounting() noexcept {modalColdCopyAccounting={};}
inline void accountModalCoefficientWrite(std::size_t count,bool factory) noexcept {
    auto& written=factory?modalColdCopyAccounting.factoryCoefficientValuesWritten:modalColdCopyAccounting.fullCoefficientValuesWritten;
    written+=count;
}
}
#define AIRACK_MODAL_COEFFICIENT_WRITE(count,factory) (::airack::space::probe::accountModalCoefficientWrite((count),(factory)))
#define AIRACK_MODAL_STATE_CLEAR(count) (::airack::space::probe::modalColdCopyAccounting.stateValuesCleared+=(count))
#else
#define AIRACK_MODAL_COEFFICIENT_WRITE(count,factory) ((void)0)
#define AIRACK_MODAL_STATE_CLEAR(count) ((void)0)
#endif
