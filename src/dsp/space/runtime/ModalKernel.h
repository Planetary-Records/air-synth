#pragma once
#include "ModalTank.h"
namespace airack::space::runtime {
using ModalFrame=ModalTank::Frame;
using ModalKernel=void(*)(const PreparedModal&,double*,double*,std::span<const ModalFrame>,std::span<ModalFrame>) noexcept;
void modalScalar(const PreparedModal&,double*,double*,std::span<const ModalFrame>,std::span<ModalFrame>) noexcept;
void modalSse2(const PreparedModal&,double*,double*,std::span<const ModalFrame>,std::span<ModalFrame>) noexcept;
bool modalSse2Available() noexcept;
ModalKernel availableModalAvx2Kernel() noexcept;
ModalKernel modalKernelFor(ModalBackend,bool haveSse2,ModalKernel avx2) noexcept;
ModalKernel specializeModalKernel(ModalKernel,const PreparedModal&,bool pairedOutput=false) noexcept;
bool modalKernelUsesPairedOutput(ModalKernel) noexcept;
}
