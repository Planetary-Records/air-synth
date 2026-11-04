#include "ModalTailMath.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
namespace airack::space::runtime {
namespace tail_detail {
bool finite(const PostCoefficients& c) noexcept {return std::isfinite(c.b0)&&std::isfinite(c.b1)&&std::isfinite(c.b2)&&std::isfinite(c.a1)&&std::isfinite(c.a2);}
std::array<std::complex<double>,2> roots(const PostCoefficients& c) noexcept {
    const double d=std::fma(c.a1,c.a1,-4*c.a2);
    if(d<0){const std::complex<double> p{-c.a1*.5,std::sqrt(-d)*.5};return {p,std::conj(p)};}
    const double q=-.5*(c.a1+std::copysign(std::sqrt(d),c.a1));return {q,q==0?0:c.a2/q};
}
double rootMargin(const PostCoefficients& c) noexcept {return 64*std::numeric_limits<double>::epsilon()*(1+std::abs(c.a1)+std::abs(c.a2));}
}
using tail_detail::finite;using tail_detail::roots;using tail_detail::rootMargin;
ModalWeightedSection boundModalPostSection(const PostCoefficients& c,double rho) noexcept {
    ModalWeightedSection result;if(!finite(c)||!std::isfinite(rho)||rho<=0||rho>1)return result;const auto p=roots(c);const double pad=rootMargin(c),r1=std::abs(p[0])+pad,r2=std::abs(p[1])+pad;
    if(!(r1<rho&&r2<rho))return result;const double h1=c.b1-c.a1*c.b0,h2=c.b2-c.a1*h1-c.a2*c.b0;
    const double error=256*std::numeric_limits<double>::epsilon()*(1+std::abs(c.b0)+std::abs(c.b1)+std::abs(c.b2)+std::abs(c.a1)+std::abs(c.a2)+std::abs(h1)+std::abs(h2));
    const double one=1-r1/rho,two=1-r2/rho,separation=std::abs(p[0]-p[1])-2*pad;
    double bound=std::abs(c.b0)+error+(std::abs(h1)+error)/(rho*one)+(std::abs(h2-p[0]*h1)+error)/(rho*rho*one*two);
    if(separation>0){const double u=(std::abs(h2-p[1]*h1)+error)/separation,v=(std::abs(p[0]*h1-h2)+error)/separation;
        bound=std::min(bound,std::abs(c.b0)+error+u/(rho*one)+v/(rho*two));}
    if(!(bound>0)||!std::isfinite(bound))return result;return {true,std::log(bound)+1e-12,std::max(r1,r2)};
}
}
