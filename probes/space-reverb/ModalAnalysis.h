#pragma once
#include "SpaceTank.h"
#include <complex>
#include <functional>
#include <vector>
#include <utility>
#include <span>
namespace airack::space::probe {
struct ModalNetwork : detail::LossNetwork {
    bool logarithmicReference=false;
    ModalNetwork()=default;
    ModalNetwork(double rate,std::vector<SpaceTank::LineView> views,std::vector<double> matrix)
        :detail::LossNetwork{rate,std::move(views),std::move(matrix),{}} {}
};
struct Mode {std::complex<double> s;double frequency=0,time=0,residual=1;};
struct ModalSearch {
    std::vector<Mode> modes;
    std::size_t seeds=0,converged=0,duplicates=0,outside=0,failed=0;
};
struct ModalCount {bool converged=false;std::size_t roots=0,samples=0;int levels=0;double maxPhaseIncrement=0,windingResidual=1;};
ModalNetwork inspectNetwork(const SpaceTank&);
ModalSearch searchModes(const ModalNetwork&,double lowHz,double highHz,const std::function<double(double)>& expectedTime,std::span<const std::complex<double>> extraSeeds={});
ModalCount countModes(const ModalNetwork&,double leftReal,double rightReal,double lowHz,double highHz);
}
