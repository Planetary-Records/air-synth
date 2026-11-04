#include "ModalAnalysis.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
namespace airack::space::probe {
namespace {
using Complex=std::complex<double>;
using Matrix=std::array<std::array<Complex,16>,16>;
bool finite(Complex z){return std::isfinite(z.real())&&std::isfinite(z.imag());}
Matrix characteristicLog(const ModalNetwork& network,Complex s)
{
    Matrix a{};const auto n=network.lines.size();const auto z=std::exp(-s/network.sampleRate),z2=z*z;
    for(std::size_t row=0;row<n;++row){
        const auto& line=network.lines[row];Complex numerator=std::log(line.loss->scalar),denominator=s*line.delaySeconds;
        for(const auto& stage:line.loss->stages){
            numerator+=std::log(stage.b0+stage.b1*z+stage.b2*z2);
            denominator+=std::log(1.+stage.a1*z+stage.a2*z2);
        }
        const double scale=std::max(numerator.real(),denominator.real());
        const auto num=std::exp(numerator-scale),den=std::exp(denominator-scale);
        for(std::size_t col=0;col<n;++col)a[row][col]=(row==col?den:Complex{})-num*network.mixing[row*n+col];
    }
    return a;
}
Matrix characteristic(const ModalNetwork& network,Complex s)
{
    if(network.poll)network.poll();
    if(network.logarithmicReference)return characteristicLog(network,s);
    Matrix a{};const auto n=network.lines.size();const auto z=std::exp(-s/network.sampleRate),z2=z*z;
    for(std::size_t row=0;row<n;++row){
        const auto& line=network.lines[row];Complex numerator=line.loss->scalar,denominator=1.;
        for(const auto& stage:line.loss->stages){
            numerator*=stage.b0+stage.b1*z+stage.b2*z2;denominator*=1.+stage.a1*z+stage.a2*z2;
            const double scale=std::max(std::abs(numerator),std::abs(denominator));numerator/=scale;denominator/=scale;
        }
        denominator*=std::exp(s*line.delaySeconds);
        const double scale=std::max(std::abs(numerator),std::abs(denominator));numerator/=scale;denominator/=scale;
        for(std::size_t col=0;col<n;++col)a[row][col]=(row==col?denominator:Complex{})-numerator*network.mixing[row*n+col];
    }
    return a;
}
Complex determinant(Matrix a,std::size_t n)
{
    Complex result=1.;
    for(std::size_t k=0;k<n;++k){
        std::size_t best=k;for(std::size_t row=k+1;row<n;++row)if(std::abs(a[row][k])>std::abs(a[best][k]))best=row;
        if(best!=k){std::swap(a[best],a[k]);result=-result;}
        const auto pivot=a[k][k];if(std::abs(pivot)==0)return {};
        if(!finite(pivot))return {std::numeric_limits<double>::quiet_NaN(),0};result*=pivot;
        for(std::size_t row=k+1;row<n;++row){const auto factor=a[row][k]/pivot;for(std::size_t col=k+1;col<n;++col)a[row][col]-=factor*a[k][col];}
    }
    return result;
}
double nullResidual(const Matrix& original,std::size_t n)
{
    auto a=original;std::array<std::size_t,16> permutation{};for(std::size_t i=0;i<n;++i)permutation[i]=i;
    std::size_t rank=n-1;
    for(std::size_t k=0;k+1<n;++k){
        std::size_t bestRow=k,bestCol=k;
        for(std::size_t row=k;row<n;++row)for(std::size_t col=k;col<n;++col)if(std::abs(a[row][col])>std::abs(a[bestRow][bestCol])){bestRow=row;bestCol=col;}
        if(std::abs(a[bestRow][bestCol])==0){rank=k;break;}
        std::swap(a[bestRow],a[k]);for(std::size_t row=0;row<n;++row)std::swap(a[row][bestCol],a[row][k]);std::swap(permutation[bestCol],permutation[k]);
        for(std::size_t row=k+1;row<n;++row){const auto factor=a[row][k]/a[k][k];a[row][k]=0;for(std::size_t col=k+1;col<n;++col)a[row][col]-=factor*a[k][col];}
    }
    std::array<Complex,16> x{},v{};x[rank]=1.;
    for(std::size_t i=rank;i-->0;){Complex sum=0;for(std::size_t j=i+1;j<n;++j)sum+=a[i][j]*x[j];x[i]=-sum/a[i][i];}
    double vectorNorm=0,matrixNorm=0,residual=0;
    for(std::size_t i=0;i<n;++i){v[permutation[i]]=x[i];vectorNorm+=std::norm(x[i]);}
    for(std::size_t i=0;i<n;++i){Complex sum=0;for(std::size_t j=0;j<n;++j){sum+=original[i][j]*v[j];matrixNorm+=std::norm(original[i][j]);}residual+=std::norm(sum);}
    return std::sqrt(residual)/(std::max(1.,std::sqrt(matrixNorm))*std::sqrt(vectorNorm));
}
}
ModalNetwork inspectNetwork(const SpaceTank& tank)
{
    ModalNetwork result;result.sampleRate=tank.sampleRate();const auto n=tank.lineCount();
    for(std::size_t i=0;i<n;++i)result.lines.push_back(tank.inspectLine(i));
    result.mixing.resize(n*n);
    const double norm=n==8?.3535533905932737622:n==16?.25:0;
    for(std::size_t i=0;i<n;++i)for(std::size_t j=0;j<n;++j){auto bits=i&j;bool odd=false;while(bits){odd=!odd;bits&=bits-1;}result.mixing[i*n+j]=odd?-norm:norm;}
    return result;
}
ModalSearch searchModes(const ModalNetwork& network,double low,double high,const std::function<double(double)>& expected,std::span<const Complex> extraSeeds)
{
    ModalSearch result;const auto n=network.lines.size();double totalDelay=0;
    if(n<1||n>16||network.mixing.size()!=n*n||network.sampleRate<8000||!std::isfinite(low)||!std::isfinite(high)||low<=0||high<=low||high>=.5*network.sampleRate)return result;
    for(const auto& line:network.lines){if(!line.loss||!(line.delaySeconds>0)||!(line.loss->scalar>0))return result;totalDelay+=line.delaySeconds;}
    const double spacing=1/totalDelay,stepHz=.25*spacing,trust=std::numbers::pi*spacing;
    const auto count=std::size_t(std::ceil((high-low)/stepHz));
    const auto refine=[&](Complex s){bool converged=false;double residual=1;
        for(int iteration=0;iteration<80;++iteration){
            const auto a=characteristic(network,s);const auto value=determinant(a,n);
            const auto derivative=(determinant(characteristic(network,s+1e-4),n)-determinant(characteristic(network,s-1e-4),n))/2e-4;
            if(!finite(value)||!finite(derivative)||std::abs(derivative)<1e-30)break;
            auto delta=value/derivative;if(!finite(delta))break;
            if(std::abs(delta)<=1e-7){residual=nullResidual(a,n);if(std::isfinite(residual)&&residual<=1e-9){converged=true;break;}}
            if(std::abs(delta)>trust)delta*=trust/std::abs(delta);
            bool improved=false;
            for(double damping:{1.,.5,.25,.125,.0625,.03125,.015625}){
                const auto candidate=s-damping*delta;if(std::abs(candidate.real())>10000||std::abs(candidate.imag())>std::numbers::pi*network.sampleRate)continue;
                const auto trial=determinant(characteristic(network,candidate),n);
                if(finite(trial)&&std::abs(trial)<std::abs(value)){s=candidate;improved=true;break;}
            }
            if(!improved)break;
        }
        if(!converged){++result.failed;return;}++result.converged;
        const double f=s.imag()/(2*std::numbers::pi);
        if(f<low||f>high||s.real()>=0){++result.outside;return;}
        if(std::any_of(result.modes.begin(),result.modes.end(),[&](const Mode& m){return std::abs(m.s-s)<1e-5;})){++result.duplicates;return;}
        result.modes.push_back({s,f,-std::log(1000.)/s.real(),residual});
    };
    for(std::size_t seed=0;seed<=count;++seed)for(double scale:{.8,1.,1.2}){
        const double frequency=std::min(high,low+seed*stepHz),time=expected(frequency);++result.seeds;
        if(!(time>0)||!std::isfinite(time)){++result.failed;continue;}
        refine({-std::log(1000.)/time*scale,2*std::numbers::pi*frequency});
    }
    for(const auto s:extraSeeds){++result.seeds;if(!finite(s)){++result.failed;continue;}refine(s);}
    std::sort(result.modes.begin(),result.modes.end(),[](const Mode& a,const Mode& b){return a.frequency<b.frequency;});return result;
}
ModalCount countModes(const ModalNetwork& network,double left,double right,double low,double high)
{
    ModalCount result;const auto n=network.lines.size();double density=0;
    if(n<1||n>16||network.mixing.size()!=n*n||!std::isfinite(network.sampleRate)||network.sampleRate<8000
        ||!std::isfinite(left)||!std::isfinite(right)||left>=right||!std::isfinite(low)||!std::isfinite(high)||low<=0||high<=low||high>=.5*network.sampleRate)return result;
    for(const auto& line:network.lines){if(!line.loss||!std::isfinite(line.delaySeconds)||line.delaySeconds<=0)return result;density+=line.delaySeconds+2*double(line.loss->stages.size())/network.sampleRate;}
    std::size_t vertical=std::max(std::size_t(32),std::size_t(std::ceil(16*(high-low)*density)));
    std::size_t horizontal=std::max(std::size_t(32),std::size_t(std::ceil(4*(right-left)*density)));
    std::size_t previousCount=0;int stable=0;
    const std::array<Complex,5> corners{{{left,2*std::numbers::pi*low},{right,2*std::numbers::pi*low},
        {right,2*std::numbers::pi*high},{left,2*std::numbers::pi*high},{left,2*std::numbers::pi*low}}};
    for(int level=0;level<13;++level){
        const std::size_t points=1+2*(vertical+horizontal);
        if(points>262144-result.samples)return result;
        double phaseSum=0,maxStep=0;bool valid=true;
        auto phaseAt=[&](Complex s,double& phase){
            if(result.samples>=262144)return false;
            ++result.samples;const auto value=determinant(characteristic(network,s),n);
            if(!finite(value)||std::abs(value)<1e-250)return false;
            phase=std::arg(value);return true;
        };
        auto integrate=[&](auto&& self,Complex start,Complex end,double startPhase,double endPhase,int depth)->bool{
            const auto middle=.5*(start+end);double middlePhase=0;if(!phaseAt(middle,middlePhase))return false;
            const double first=std::remainder(middlePhase-startPhase,2*std::numbers::pi),second=std::remainder(endPhase-middlePhase,2*std::numbers::pi);
            if(std::abs(first)<=std::numbers::pi/8&&std::abs(second)<=std::numbers::pi/8&&std::abs(first-second)<=std::numbers::pi/16){
                phaseSum+=first+second;maxStep=std::max({maxStep,std::abs(first),std::abs(second)});return true;
            }
            return depth<20&&self(self,start,middle,startPhase,middlePhase,depth+1)&&self(self,middle,end,middlePhase,endPhase,depth+1);
        };
        Complex previous=corners[0];double previousPhase=0;valid=phaseAt(previous,previousPhase);
        for(std::size_t edge=0;edge<4&&valid;++edge){const auto steps=edge%2?vertical:horizontal;
            for(std::size_t i=1;i<=steps&&valid;++i){const auto point=corners[edge]+(corners[edge+1]-corners[edge])*(double(i)/steps);double phase=0;
                valid=phaseAt(point,phase)&&integrate(integrate,previous,point,previousPhase,phase,0);previous=point;previousPhase=phase;}}
        ++result.levels;if(!valid)return result;
        const double winding=phaseSum/(2*std::numbers::pi),rounded=std::round(winding);
        result.maxPhaseIncrement=maxStep;result.windingResidual=std::abs(winding-rounded);
        if(rounded<0||rounded>1000000)return result;
        result.roots=std::size_t(rounded);
        if(result.windingResidual<=1e-6&&maxStep<=std::numbers::pi/8){stable=stable>0&&previousCount==result.roots?stable+1:1;}else stable=0;
        previousCount=result.roots;
        if(stable>=3){result.converged=true;return result;}
        vertical*=2;horizontal*=2;
    }
    return result;
}
}
