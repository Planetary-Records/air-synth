#include "PatternTiming.h"
#include <algorithm>
#include <cmath>
namespace airack {
bool makePatternPlaybackNotes(const PatternDocument& document, PatternPlaybackNotes& output) noexcept {
    if (!validatePatternDocument(document)) return false;
    PatternPlaybackNotes result;
    result.lengthBeats=patternLengthBeats(document);
    result.noteCount=document.noteCount;
    const auto grid=patternGridBeats(document.grid);
    for (std::size_t i=0;i<document.noteCount;++i) {
        const auto& note=document.notes[i];
        const auto pairStart=std::floor(note.startBeats/(2*grid))*(2*grid);
        const auto pairEnd=std::min(pairStart+2*grid,result.lengthBeats);
        const auto second=pairEnd-pairStart-grid;
        double start=note.startBeats;
        if (second>0 && document.swing!=0) {
            const auto displacement=.5*document.swing*std::min(grid,second);
            const auto local=note.startBeats-pairStart;
            start=pairStart+(local<grid ? local*(grid+displacement)/grid
                : grid+displacement+(local-grid)*(second-displacement)/second);
        }
        start=std::min(start,std::nextafter(result.lengthBeats,0.));
        result.notes[i]={start,std::min(result.lengthBeats,start+note.durationBeats),note.pitch,note.velocity,note.channel};
    }
    output=result;
    return true;
}
}
