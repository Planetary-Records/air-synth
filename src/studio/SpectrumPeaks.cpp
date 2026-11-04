#include "SpectrumPeaks.h"
#include <algorithm>
#include <cmath>

namespace airack::studio
{
SpectrumPeaks findSpectrumPeaks(const FxSpectrum::Result& result,bool output) noexcept
{
    SpectrumPeaks selected;
    if(!result.valid||result.generation==0||result.windows==0||!std::isfinite(result.sampleRate)
        ||result.sampleRate<8000||result.sampleRate>1536000)return selected;
    const auto& levels=output?result.outputDb:result.inputDb;
    for(const auto level:levels)if(!std::isfinite(level)||level<FxSpectrum::floorDb||level>120)return selected;
    const double binHz=result.sampleRate/FxSpectrum::fftSize,maximumHz=std::min(20000.,.49*result.sampleRate);
    // Eight bounded scans implement greedy prominence ordering without a heap
    // or a candidate array. Each peak walks only to its adjacent valleys.
    for(std::size_t rank=0;rank<selected.values.size();++rank){
        SpectrumPeak best;double bestProminence=0;bool found=false;
        for(std::size_t first=1;first+1<levels.size();){
            if(levels[first]<=levels[first-1]){++first;continue;}
            auto last=first;while(last+1<levels.size()&&levels[last+1]==levels[first])++last;
            if(last+1==levels.size()||levels[last]<=levels[last+1]){first=last+1;continue;}
            double center=.5*double(first+last),peakDb=levels[first];
            if(first==last){
                const double left=levels[first-1],middle=levels[first],right=levels[first+1];
                const double denominator=left-2*middle+right;
                const double shift=denominator<-1.e-12?std::clamp(.5*(left-right)/denominator,-.5,.5):0.;
                center=double(first)+shift;peakDb=middle-.25*(left-right)*shift;
            }
            const double frequency=center*binHz;
            bool separated=frequency>=20&&frequency<=maximumHz;
            for(std::size_t n=0;separated&&n<selected.size;++n)
                separated=std::abs(std::log2(frequency/selected.values[n].frequencyHz))>=1./12.;
            if(separated){
                auto leftValley=first-1,rightValley=last+1;
                while(leftValley>0&&levels[leftValley-1]<=levels[leftValley])--leftValley;
                while(rightValley+1<levels.size()&&levels[rightValley+1]<=levels[rightValley])++rightValley;
                const double prominence=peakDb-std::max(levels[leftValley],levels[rightValley]);
                if(prominence>=6){
                    const double crossingDb=peakDb-3;
                    auto left=first,right=last;
                    while(left>leftValley&&levels[left]>crossingDb)--left;
                    while(right<rightValley&&levels[right]>crossingDb)++right;
                    if(levels[left]<=crossingDb&&levels[right]<=crossingDb&&left<first&&right>last){
                        const double leftCross=double(left)+(crossingDb-levels[left])/(levels[left+1]-levels[left]);
                        const double rightCross=double(right-1)+(levels[right-1]-crossingDb)/(levels[right-1]-levels[right]);
                        const double width=(rightCross-leftCross)*binHz;
                        if(width>0&&(!found||prominence>bestProminence
                            ||(prominence==bestProminence&&frequency<best.frequencyHz))){
                            best={frequency,float(peakDb),float(std::clamp(frequency/width,.25,18.)),float(prominence)};bestProminence=prominence;found=true;
                        }
                    }
                }
            }
            first=last+1;
        }
        if(!found)break;
        selected.values[selected.size++]=best;
    }
    return selected;
}
}
