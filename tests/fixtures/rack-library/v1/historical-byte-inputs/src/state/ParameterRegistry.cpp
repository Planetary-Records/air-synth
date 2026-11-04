#include "ParameterRegistry.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace airack
{
namespace
{
constexpr std::size_t commonStart = 12, bankStart = 28, bankSize = 23, wavetableStart = 120;
// Plain compile-time field data is shared by the catalog and numeric mapping.
// Render never touches the strings/vectors in the lazily constructed catalog.
struct Field
{
    const char* id;
    const char* name;
    const char* unit;
    float minimum, maximum, initial, skew;
    int choices = 0;
};
constexpr Field output[] {
    {"gain_db", "Output gain", "dB", -24, 36, 30, 1},
    {"protection", "Output protection (-1 dBFS)", "", 0, 1, 1, 1, 2},
    {"render_quality", "Render quality", "", 0, 1, 0, 1, 2}
};
constexpr Field master {"master_level", "Master level", "", 0, 1, .5f, 1};
constexpr Field macro {"", "Macro", "", 0, 1, 0, 1};
constexpr Field common[] {
    {"enabled", "Enabled", "", 0, 1, 0, 1, 2},
    {"engine", "Engine", "", 0, 1, 0, 1, 2},
    {"level", "Level", "", 0, 1, .5f, 1},
    {"pan", "Pan", "", -1, 1, 0, 1}
};
constexpr Field analogOscillator[] {
    {"waveform", "Waveform", "", 0, 3, 2, 1, 4},
    {"tune", "Tune", "st", -24, 24, 0, 1},
    {"detune", "Detune", "cent", -100, 100, 0, 1},
    {"level", "Level", "", 0, 1, .5f, 1},
    {"pulse_width", "Pulse width", "", .05f, .95f, .5f, 1}
};
constexpr Field wavetableOscillator[] {
    {"table", "Table", "", 0, 1, 0, 1, 2},
    {"position", "Position", "", 0, 1, 0, 1},
    {"tune", "Tune", "st", -24, 24, 0, 1},
    {"detune", "Detune", "cent", -100, 100, 0, 1},
    {"level", "Level", "", 0, 1, .5f, 1}
};
constexpr Field bankTail[] {
    {"noise", "Noise", "", 0, 1, 0, 1},
    {"filter.cutoff", "Filter cutoff", "Hz", 20, 20000, 4000, .25f},
    {"filter.resonance", "Filter resonance", "", 0, .95f, .25f, 1},
    {"filter.mode", "Filter mode", "", 0, 2, 0, 1, 3},
    {"amp.attack", "Amp attack", "s", 0, 20, .005f, .3f},
    {"amp.decay", "Amp decay", "s", 0, 20, .15f, .3f},
    {"amp.sustain", "Amp sustain", "", 0, 1, .65f, 1},
    {"amp.release", "Amp release", "s", 0, 20, .15f, .3f},
    {"filter_env.attack", "Filter envelope attack", "s", 0, 20, .005f, .3f},
    {"filter_env.decay", "Filter envelope decay", "s", 0, 20, .15f, .3f},
    {"filter_env.sustain", "Filter envelope sustain", "", 0, 1, .65f, 1},
    {"filter_env.release", "Filter envelope release", "s", 0, 20, .15f, .3f},
    {"filter_env.octaves", "Filter envelope amount", "oct", -8, 8, 0, 1}
};
constexpr Field performance[] {
    {"voice_mode","Voice mode","",0,1,0,1,2},
    {"glide_seconds","Glide","s",0,2,0,1},
    {"legato","Legato","",0,1,0,1,2}
};
constexpr Field gates[] {{"mute","Mute","",0,1,0,1,2},{"solo","Solo","",0,1,0,1,2}};
constexpr Field lfo[] {
    {"shape","Shape","",0,3,0,1,4},{"synced","Synced","",0,1,0,1,2},
    {"rate_hz","Rate","Hz",.05f,40,1,1},{"beats","Period","beats",.0625f,16,1,1}
};
constexpr Field route[] {
    {"enabled","Enabled","",0,1,0,1,2},{"source","Source","",0,6,0,1,7},
    {"target","Target","",0,16,0,1,17},{"bank","Bank","",0,2,0,1,3},{"amount","Amount","",-1,1,0,1}
};
constexpr Field effects[] {
    {"bypassed","Bypassed","",0,1,0,1,2},
    {"drive.gain_db","Drive gain","dB",0,24,6,1},{"drive.shape","Drive shape","",0,1,0,1,2},
    {"drive.tone_hz","Drive tone","Hz",200,20000,12000,1},{"drive.mix","Drive mix","",0,1,.5f,1},
    {"delay.synced","Delay synced","",0,1,0,1,2},{"delay.seconds","Delay time","s",.001f,4,.25f,1},
    {"delay.beats","Delay period","beats",.0625f,1,.5f,1},{"delay.feedback","Delay feedback","",0,.95f,.35f,1},
    {"delay.stereo","Delay stereo","",0,1,0,1},{"delay.filter_hz","Delay filter","Hz",20,20000,12000,1},{"delay.mix","Delay mix","",0,1,.25f,1},
    {"reverb.size","Reverb size","",0,1,.5f,1},{"reverb.decay_seconds","Reverb decay","s",.1f,20,2,1},
    {"reverb.damping","Reverb damping","",0,1,.5f,1},{"reverb.predelay_ms","Reverb predelay","ms",0,250,0,1},{"reverb.mix","Reverb mix","",0,1,.25f,1},
    {"compressor.threshold_db","Compressor threshold","dB",-60,0,-18,1},{"compressor.ratio","Compressor ratio","",1,20,4,1},
    {"compressor.attack_ms","Compressor attack","ms",.1f,100,10,1},{"compressor.release_ms","Compressor release","ms",10,2000,100,1},
    {"compressor.makeup_db","Compressor makeup","dB",0,24,0,1},{"compressor.mix","Compressor mix","",0,1,1,1}
};
constexpr Field equalizer[] {
    {"eq.low_shelf.frequency_hz","EQ low shelf frequency","Hz",20,2000,160,.35f},
    {"eq.low_shelf.gain_db","EQ low shelf gain","dB",-18,18,0,1},
    {"eq.low_shelf.q","EQ low shelf Q","",.25f,4,.70710678f,1},
    {"eq.bell.frequency_hz","EQ bell frequency","Hz",20,20000,1000,.35f},
    {"eq.bell.gain_db","EQ bell gain","dB",-18,18,0,1},
    {"eq.bell.q","EQ bell Q","",.25f,4,1,1},
    {"eq.high_shelf.frequency_hz","EQ high shelf frequency","Hz",1000,20000,8000,.35f},
    {"eq.high_shelf.gain_db","EQ high shelf gain","dB",-18,18,0,1},
    {"eq.high_shelf.q","EQ high shelf Q","",.25f,4,.70710678f,1}
};
constexpr Field filterFx[] {
    {"filter_fx.mode","Filter mode","",0,2,0,1,3},
    {"filter_fx.cutoff_hz","Filter cutoff","Hz",20,20000,4000,.35f},
    {"filter_fx.resonance","Filter resonance","Q",.5f,10,.70710678f,1},
    {"filter_fx.drive_db","Filter drive","dB",0,24,0,1},
    {"filter_fx.mix","Filter mix","",0,1,1,1}
};
constexpr Field chorusFx[] {
    {"chorus.rate_hz","Chorus rate","Hz",.1f,5,.35f,1},
    {"chorus.depth","Chorus depth","",0,1,.25f,1},
    {"chorus.base_delay_ms","Chorus base delay","ms",12,40,18,1},
    {"chorus.feedback","Chorus feedback","",-.8f,.8f,.1f,1},
    {"chorus.mix","Chorus mix","",0,1,.25f,1}
};
constexpr Field phaserFx[] {
    {"phaser.rate_hz","Phaser rate","Hz",.1f,5,.35f,1},
    {"phaser.depth","Phaser depth","",0,1,.6f,1},
    {"phaser.centre_hz","Phaser centre","Hz",20,20000,1300,.35f},
    {"phaser.feedback","Phaser feedback","",-.8f,.8f,.2f,1},
    {"phaser.mix","Phaser mix","",0,1,.5f,1}
};
constexpr Field flangerFx[] {
    {"flanger.rate_hz","Flanger rate","Hz",.05f,5,.25f,1},
    {"flanger.depth","Flanger depth","",0,1,.6f,1},
    {"flanger.base_delay_ms","Flanger delay","ms",1,8,2,1},
    {"flanger.feedback","Flanger feedback","",-.8f,.8f,.35f,1},
    {"flanger.mix","Flanger mix","",0,1,.5f,1}
};
constexpr Field stereoFx[] {
    {"stereo.gain_db","Stereo gain","dB",-24,12,0,1},
    {"stereo.pan","Stereo balance","",-1,1,0,1},
    {"stereo.width","Stereo width","",0,2,1,1},
    {"stereo.channel_mode","Stereo channels","",0,2,0,1,3}
};
constexpr Field dimensionFx[] {
    {"dimension.depth","Dimension depth","",0,1,.6f,1},
    {"dimension.rate_hz","Dimension rate","Hz",.05f,2,.35f,1},
    {"dimension.width","Dimension width","",0,2,1,1},
    {"dimension.mix","Dimension mix","",0,1,.5f,1}
};
constexpr Field frequencyShiftFx[] {
    {"frequency_shift.hz","Frequency shift","Hz",-1000,1000,7,1},
    {"frequency_shift.stereo_offset_hz","Frequency stereo offset","Hz",-100,100,0,1},
    {"frequency_shift.mix","Frequency shift mix","",0,1,.25f,1}
};
std::array<std::pair<std::uint32_t,std::uint32_t>,parameterCount> numericKeys {};
constexpr Field convolutionFx[] {
    {"convolution.ir","Convolution IR","",0,1,1,1,2},
    {"convolution.predelay_ms","Convolution predelay","ms",0,250,0,1},
    {"convolution.wet_trim_db","Convolution wet trim","dB",-24,12,0,1},
    {"convolution.mix","Convolution mix","",0,1,.25f,1}
};
constexpr Field frequencySplitFx[] {
    {"frequency_split.crossover_hz","Split crossover","Hz",20,20000,1000,.25f},
    {"frequency_split.low_gain_db","Split low gain","dB",-24,12,0,1},
    {"frequency_split.low_mute","Split low mute","",0,1,0,1,2},
    {"frequency_split.low_tone","Split low tone","",-1,1,0,1},
    {"frequency_split.high_gain_db","Split high gain","dB",-24,12,0,1},
    {"frequency_split.high_mute","Split high mute","",0,1,0,1,2},
    {"frequency_split.high_tone","Split high tone","",-1,1,0,1},
    {"frequency_split.mix","Split mix","",0,1,1,1}
};
constexpr Field midSideSplitFx[] {
    {"mid_side_split.mid_gain_db","Mid gain","dB",-24,12,0,1},
    {"mid_side_split.mid_mute","Mid mute","",0,1,0,1,2},
    {"mid_side_split.mid_tone","Mid tone","",-1,1,0,1},
    {"mid_side_split.side_gain_db","Side gain","dB",-24,12,0,1},
    {"mid_side_split.side_mute","Side mute","",0,1,0,1,2},
    {"mid_side_split.side_tone","Side tone","",-1,1,0,1},
    {"mid_side_split.mix","Mid/side mix","",0,1,1,1}
};
constexpr Field dynamicEqBand[] {
    {"active","Active","",0,1,0,1,2},
    {"bypassed","Bypassed","",0,1,0,1,2},
    {"shape","Shape","",0,5,0,1,6},
    {"frequency_hz","Frequency","Hz",20,20000,1000,.35f},
    {"gain_db","Gain","dB",-24,24,0,1},
    {"q","Q","",.25f,18,1,.5f},
    {"slope","Cut slope","dB/oct",0,1,0,1,2},
    {"range_db","Dynamic range","dB",-24,24,0,1},
    {"threshold_db","Threshold","dB",-60,0,-24,1},
    {"attack_ms","Attack","ms",.1f,200,10,.4f},
    {"release_ms","Release","ms",10,2000,100,.4f}
};
constexpr Field dynamicEqOutput {"output_gain_db","Dynamic EQ output","dB",-24,12,0,1};
constexpr Field dynamicEqGain[] {
    {"auto_gain","Auto Gain","",0,1,0,1,2},
    {"gain_scale","Gain scale","x",0,2,1,1}
};
constexpr Field dynamicEqPlacement {"placement","Placement","",0,4,0,1,5};
constexpr Field saturationFx[] {
    {"saturation.mode","Mode","",0,1,0,1,2},
    {"saturation.input_trim_db","Input trim","dB",-24,24,0,1},
    {"saturation.output_trim_db","Output trim","dB",-24,12,0,1},
    {"saturation.mix","Global mix","",0,1,1,1},
    {"saturation.drive_compensation","Drive compensation","",0,1,1,1,2},
    {"saturation.low_crossover_hz","Low crossover","Hz",40,2000,180,.35f},
    {"saturation.high_crossover_hz","High crossover","Hz",500,16000,3500,.35f},
    {"saturation.pre_low_cut_enabled","Pre low cut enabled","",0,1,0,1,2},
    {"saturation.pre_low_cut_hz","Pre low cut","Hz",20,2000,20,.35f},
    {"saturation.post_high_cut_enabled","Post high cut enabled","",0,1,0,1,2},
    {"saturation.post_high_cut_hz","Post high cut","Hz",1000,20000,20000,.35f},
    {"saturation.full.bypassed","Full bypass","",0,1,0,1,2},
    {"saturation.full.character","Full character","",0,2,0,1,3},
    {"saturation.full.drive_db","Full drive","dB",0,36,0,1},
    {"saturation.full.pre_tilt_db","Full pre tilt","dB",-6,6,0,1},
    {"saturation.full.mix","Full mix","",0,1,1,1},
    {"saturation.full.trim_db","Full trim","dB",-18,18,0,1},
    {"saturation.low.bypassed","Low bypass","",0,1,0,1,2},
    {"saturation.low.character","Low character","",0,2,0,1,3},
    {"saturation.low.drive_db","Low drive","dB",0,36,0,1},
    {"saturation.low.pre_tilt_db","Low pre tilt","dB",-6,6,0,1},
    {"saturation.low.mix","Low mix","",0,1,1,1},
    {"saturation.low.trim_db","Low trim","dB",-18,18,0,1},
    {"saturation.mid.bypassed","Mid bypass","",0,1,0,1,2},
    {"saturation.mid.character","Mid character","",0,2,0,1,3},
    {"saturation.mid.drive_db","Mid drive","dB",0,36,0,1},
    {"saturation.mid.pre_tilt_db","Mid pre tilt","dB",-6,6,0,1},
    {"saturation.mid.mix","Mid mix","",0,1,1,1},
    {"saturation.mid.trim_db","Mid trim","dB",-18,18,0,1},
    {"saturation.high.bypassed","High bypass","",0,1,0,1,2},
    {"saturation.high.character","High character","",0,2,0,1,3},
    {"saturation.high.drive_db","High drive","dB",0,36,0,1},
    {"saturation.high.pre_tilt_db","High pre tilt","dB",-6,6,0,1},
    {"saturation.high.mix","High mix","",0,1,1,1},
    {"saturation.high.trim_db","High trim","dB",-18,18,0,1},
};
static_assert(std::size(saturationFx) == saturationParametersPerSlot);
constexpr Field delayTape[] {
    {"delay.saturation","Delay saturation","",0,1,0,1},
    {"delay.low_cut_hz","Delay low cut","Hz",20,2000,20,1},
    {"delay.wobble","Delay wobble","",0,1,0,1},
    {"delay.wobble_rate_hz","Delay wobble rate","Hz",.1f,10,.8f,1},
    {"delay.glide","Delay glide","",0,1,0,1,2}
};
constexpr Field dynamicEqExtension[] {
    {"shape_extension","Shape extension","",0,3,0,1,4},
    {"cut_slope_extension","Cut slope extension","",0,4,0,1,5}
};
constexpr Field delayStereo[] {
    {"delay.right_ratio","Delay right time","x",.25f,4,1,1},
    {"delay.ping_pong","Delay ping-pong","",0,1,0,1,2},
    {"delay.note","Delay note","",0,21,0,1,22}
};
constexpr Field dynamicEqDetector[] {
    {"auto_threshold","Auto threshold","",0,1,0,1,2},
    {"adaptive_timing","Adaptive timing","",0,1,0,1,2},
    {"detector_source","Detector source","",0,3,0,1,4},
    {"detector_low_hz","Detector low","Hz",20,20000,20,.35f},
    {"detector_high_hz","Detector high","Hz",20,20000,20000,.35f}
};
constexpr Field spaceReverbGlobals[] {
    {"size","Space size","",0,1,.5f,1},
    {"decay_seconds","Space decay","s",.1f,20,2,.4f},
    {"predelay_ms","Space predelay","ms",0,250,20,1},
    {"brightness","Space brightness","",0,1,.5f,1},
    {"character","Space character","",0,1,.25f,1},
    {"distance","Space distance","",0,1,.5f,1},
    {"width","Space width","",0,2,1,1},
    {"mix","Space mix","",0,1,.25f,1},
    {"duck_range_db","Space duck range","dB",0,24,0,1},
    {"duck_threshold_db","Space duck threshold","dB",-60,0,-24,1},
    {"duck_attack_ms","Space duck attack","ms",.1f,200,10,.4f},
    {"duck_release_ms","Space duck release","ms",10,2000,250,.4f}
};
constexpr Field spaceReverbDecayBand[] {
    {"active","Active","",0,1,0,1,2},
    {"bypassed","Bypassed","",0,1,0,1,2},
    {"shape","Shape","",0,2,0,1,3},
    {"freq_hz","Frequency","Hz",20,20000,1000,.35f},
    {"multiplier","Time multiplier","x",.25f,4,1,.5f},
    {"q","Q","",.25f,4,1,.5f}
};
constexpr Field spaceReverbPostBand[] {
    {"active","Active","",0,1,0,1,2},
    {"bypassed","Bypassed","",0,1,0,1,2},
    {"shape","Shape","",0,4,0,1,5},
    {"freq_hz","Frequency","Hz",20,20000,1000,.35f},
    {"gain_db","Gain","dB",-18,18,0,1},
    {"bell_q","Bell Q","",.25f,12,1,.5f},
    {"shelf_q","Shelf Q","",.25f,4,1,.5f},
    {"slope","Cut slope","dB/oct",0,1,0,1,2}
};
constexpr Field eraReverbFx[] {
    {"voicing","Era voicing","",0,5,0,1,6},
    {"age","Era age","",0,2,0,1,3},
    {"decay_seconds","Era decay","s",.2f,70,4,.2977f},
    {"predelay_ms","Era pre-delay","ms",0,500,20,.4307f},
    {"size","Era size","",0,1,1,1},
    {"attack","Era attack","",0,1,.5f,1},
    {"diffusion","Era diffusion","",0,1,1,1},
    {"wobble","Era wobble","",0,1,.38f,1},
    {"wobble_rate_hz","Era wobble rate","Hz",.1f,10,2.53f,1},
    {"grit","Era grit","",0,1,0,1},
    {"bass_multiplier","Era bass","x",.25f,4,1.5f,.4307f},
    {"damping_hz","Era damping","Hz",100,20000,6000,.5701f},
    {"low_cut_hz","Era low cut","Hz",10,1500,10,1},
    {"high_cut_hz","Era high cut","Hz",100,20000,8000,.5701f},
    {"mix","Era mix","",0,1,.25f,1}
};
constexpr Field dynamicEqProcessingMode {"processing_mode","EQ processing mode","",0,1,0,1,2};
constexpr Field dynamicEqSpectral[] {
    {"spectral_enabled","Spectral dynamics","",0,1,0,1,2},
    {"spectral_density","Spectral density","",0,1,.5f,1},
    {"spectral_tilt_db_oct","Spectral threshold tilt","dB/oct",-6,6,0,1}
};
// Vowel size's skew puts 1 at mid-travel.
constexpr Field wavetableWarmth[] {
    {"osc.1.unison","Osc 1 unison","voices",0,6,0,1,7},
    {"osc.1.unison_cents","Osc 1 unison spread","cent",0,100,10,1},
    {"osc.2.unison","Osc 2 unison","voices",0,6,0,1,7},
    {"osc.2.unison_cents","Osc 2 unison spread","cent",0,100,10,1},
    {"unison_width","Unison width","",0,1,1,1},
    {"drift","Drift","",0,1,0,1},
    {"filter.type","Filter type","",0,3,0,1,4},
    {"filter.vowel_size","Vowel size","x",.5f,2,1,.63092975f},
    {"amp.attack_curve","Amp attack curve","",-1,1,.25f,1}
};
static_assert(std::size(wavetableWarmth)==wavetableWarmthParametersPerSlot);
constexpr Field drumMode {"drum_mode","Drum mode","",0,1,0,1,2};
constexpr Field drumPad[] {
    {"gain_db","Gain","dB",-60,12,0,1},{"pan","Pan","",-1,1,0,1},
    {"tune_semitones","Tune","st",-24,24,0,1},{"trim_start","Trim start","",0,1,0,1},
    {"trim_end","Trim end","",0,1,1,1},{"attack_seconds","Attack","s",0,1,0,.3f},
    {"decay_seconds","Decay","s",0,10,0,.3f},{"midi_note","MIDI note","",0,127,36,1,128},
    {"choke_group","Choke group","",0,16,0,1,17}
};
static_assert(std::size(drumPad)==drumParametersPerPad);
const Field& fieldAt(std::size_t index) noexcept
{
    if(index>=drumModeParameterStart&&index<saturationParameterStart)return drumMode;
    if(index>=drumParameterStart&&index<drumModeParameterStart)return drumPad[(index-drumParameterStart)%drumParametersPerPad];
    if (index == 0) return master;
    if (index < 9) return macro;
    if (index < commonStart) return performance[index-9];
    if (index >= saturationParameterStart) return saturationFx[(index-saturationParameterStart)%saturationParametersPerSlot];
    if (index >= eraReverbParameterStart) return eraReverbFx[(index-eraReverbParameterStart)%eraReverbParametersPerSlot];
    if (index >= wavetableWarmthParameterStart) return wavetableWarmth[(index-wavetableWarmthParameterStart)%wavetableWarmthParametersPerSlot];
    if (index >= spaceReverbParameterStart) {
        const auto field=(index-spaceReverbParameterStart)%spaceReverbParametersPerSlot;
        return field<12?spaceReverbGlobals[field]:field<48?spaceReverbDecayBand[(field-12)%6]:spaceReverbPostBand[(field-48)%8];
    }
    if (index >= dynamicEqSpectralParameterStart) {
        const auto local=(index-dynamicEqSpectralParameterStart)%dynamicEqSpectralParametersPerSlot;
        return local==0?dynamicEqProcessingMode:dynamicEqSpectral[(local-1)%dynamicEqSpectralParametersPerBand];
    }
    if (index >= dynamicEqDetectorParameterStart) return dynamicEqDetector[(index-dynamicEqDetectorParameterStart)%dynamicEqDetectorParametersPerBand];
    if (index >= delayStereoParameterStart) return delayStereo[(index-delayStereoParameterStart)%delayStereoParametersPerSlot];
    if (index >= dynamicEqExtensionParameterStart) return dynamicEqExtension[(index-dynamicEqExtensionParameterStart)%dynamicEqExtensionParametersPerBand];
    if (index >= delayTapeParameterStart) return delayTape[(index-delayTapeParameterStart)%delayTapeParametersPerSlot];
    if (index >= dynamicEqPlacementParameterStart) return dynamicEqPlacement;
    if (index >= dynamicEqGainParameterStart) return dynamicEqGain[(index-dynamicEqGainParameterStart)%dynamicEqGainParametersPerSlot];
    if (index >= dynamicEqParameterStart) {
        const auto field=(index-dynamicEqParameterStart)%dynamicEqParametersPerSlot;
        return field==88?dynamicEqOutput:dynamicEqBand[field%dynamicEqParametersPerBand];
    }
    if (index >= midSideSplitParameterStart) return midSideSplitFx[(index-midSideSplitParameterStart)%midSideSplitParametersPerSlot];
    if (index >= frequencySplitParameterStart) return frequencySplitFx[(index-frequencySplitParameterStart)%frequencySplitParametersPerSlot];
    if (index >= convolutionParameterStart) return convolutionFx[(index-convolutionParameterStart)%convolutionParametersPerSlot];
    if (index >= frequencyShiftParameterStart) return frequencyShiftFx[(index-frequencyShiftParameterStart)%frequencyShiftParametersPerSlot];
    if (index >= dimensionParameterStart) return dimensionFx[(index-dimensionParameterStart)%dimensionParametersPerSlot];
    if (index >= stereoParameterStart) return stereoFx[(index-stereoParameterStart)%stereoParametersPerSlot];
    if (index >= flangerParameterStart) return flangerFx[(index-flangerParameterStart)%flangerParametersPerSlot];
    if (index >= phaserParameterStart) return phaserFx[(index-phaserParameterStart)%phaserParametersPerSlot];
    if (index >= chorusParameterStart) return chorusFx[(index-chorusParameterStart)%chorusParametersPerSlot];
    if (index >= filterParameterStart) return filterFx[(index-filterParameterStart)%filterParametersPerSlot];
    if (index >= eqParameterStart) return equalizer[(index-eqParameterStart)%eqParametersPerSlot];
    if (index >= 756) return output[index-756];
    if (index >= 572) return effects[(index-572)%23];
    if (index >= 252) return route[(index-252)%5];
    if (index >= 220) return lfo[(index-220)%4];
    if (index >= 212) return gates[(index-212)%2];
    if (index < bankStart) return common[(index - commonStart) % 4];
    const auto offset = (index - bankStart) % bankSize;
    if (offset >= 10) return bankTail[offset - 10];
    return index < wavetableStart ? analogOscillator[offset % 5] : wavetableOscillator[offset % 5];
}
juce::NormalisableRange<float> range(const Field& f) noexcept
{ return {f.minimum, f.maximum, 0.0f, f.skew}; }
float physical(const ParameterValues& values, std::size_t index) noexcept
{
    const auto& f = fieldAt(index);
    // Tolerance permits serialization roundoff, but enum selection is canonical.
    if (f.choices != 0) return std::round(values[index] * static_cast<float>(f.choices - 1));
    return range(f).convertFrom0to1(values[index]);
}
std::uint32_t stateKey(const juce::String& id)
{
    std::uint32_t hash = 2166136261u;
    for (const auto* p = id.toRawUTF8(); *p != '\0'; ++p)
        hash = (hash ^ static_cast<unsigned char>(*p)) * 16777619u;
    return hash;
}
}

std::span<const ParameterDescriptor> parameterRegistry()
{
    static const auto descriptors = [] {
        std::array<ParameterDescriptor, parameterCount> result;
        for (std::size_t i = 0; i < parameterCount; ++i)
        {
            const auto& f = fieldAt(i);
            auto& d = result[i];
            d = {{}, {}, f.unit, f.choices == 0 ? ParameterKind::continuous : ParameterKind::choice,
                 f.minimum, f.maximum, f.initial, f.skew, {}, 0};
            if(i>=drumModeParameterStart&&i<saturationParameterStart){
                const auto slot=i-drumModeParameterStart+1;
                d.id="synth."+juce::String(slot)+".drum_mode";d.name="Synth "+juce::String(slot)+" Drum mode";
                d.kind=ParameterKind::toggle;d.choices={"Off","On"};
            }
            else if(i>=drumParameterStart&&i<drumModeParameterStart){
                const auto local=i-drumParameterStart,slot=local/drumParametersPerSlot+1;
                const auto pad=(local%drumParametersPerSlot)/drumParametersPerPad+1,field=local%drumParametersPerPad;
                d.id="synth."+juce::String(slot)+".drum.pad."+juce::String(pad)+"."+f.id;
                d.name="Synth "+juce::String(slot)+" Drum pad "+juce::String(pad)+" "+f.name;
                if(field==7)d.defaultValue=float(35+pad);
                if(f.choices)for(int choice=0;choice<f.choices;++choice)d.choices.push_back(field==8&&choice==0?juce::String("Off"):juce::String(choice));
            }
            else if(i==0) { d.id = "global.master_level"; d.name = f.name; }
            else if (i < 9) { d.id = "macro." + juce::String(i); d.name = "Macro " + juce::String(i); }
            else if (i < commonStart) {
                d.id="global."+juce::String(f.id); d.name=f.name;
                if(i==9)d.choices={"Polyphonic","Monophonic"};
                if(i==11){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
            }
            else if (i >= saturationParameterStart) {
                const auto slot=(i-saturationParameterStart)/saturationParametersPerSlot+1;
                const auto local=(i-saturationParameterStart)%saturationParametersPerSlot;
                d.id="effect."+juce::String(slot)+"."+f.id; d.name="Effect "+juce::String(slot)+" "+f.name;
                if(local==0) d.choices={"Full band","Three band"};
                else if(local>=saturationGlobalParameters && (local-saturationGlobalParameters)%saturationParametersPerLane==1)
                    d.choices={"Soft","Glow","Dense"};
                else if(f.choices){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
            }
            else if (i >= eraReverbParameterStart) {
                const auto slot=(i-eraReverbParameterStart)/eraReverbParametersPerSlot+1;
                const auto field=(i-eraReverbParameterStart)%eraReverbParametersPerSlot;
                d.id="effect."+juce::String(slot)+".era_reverb."+f.id;
                d.name="Effect "+juce::String(slot)+" "+f.name;
                if(field==0)d.choices={"Hall","Space","Tape","Plate","Room","Gate"};
                else if(field==1)d.choices={"Dark","Warm","Clear"};
            }
            else if (i >= wavetableWarmthParameterStart) {
                const auto local=i-wavetableWarmthParameterStart,field=local%wavetableWarmthParametersPerSlot;
                const auto slot=juce::String(local/wavetableWarmthParametersPerSlot+1);
                d.id="synth."+slot+".wavetable."+f.id;d.name="Synth "+slot+" Wavetable "+f.name;
                if(field==0||field==2)d.choices={"1","2","3","4","5","6","7"};
                else if(field==6)d.choices={"Clean","Ladder","Classic","Vowel"};
            }
            else if (i >= spaceReverbParameterStart) {
                const auto slot=(i-spaceReverbParameterStart)/spaceReverbParametersPerSlot+1;
                const auto local=(i-spaceReverbParameterStart)%spaceReverbParametersPerSlot;
                const auto decay=local>=12 && local<48;
                const auto band=local<12?0:local<48?(local-12)/6+1:(local-48)/8+1;
                const auto field=local<12?0:local<48?(local-12)%6:(local-48)%8;
                const auto family=decay?"Decay ":"Post EQ ";
                const auto key=local<12?juce::String(f.id):juce::String(decay?"decay.":"post.")+juce::String(band)+"."+f.id;
                d.id="effect."+juce::String(slot)+".space_reverb."+key;
                d.name="Effect "+juce::String(slot)+" "+(local<12?juce::String(f.name):juce::String(family)+juce::String(band)+" "+f.name);
                if(local>=12 && field==2)d.choices=decay?std::vector<juce::String>{"Bell","Low shelf","High shelf"}:std::vector<juce::String>{"Bell","Low shelf","High shelf","Low cut","High cut"};
                else if(local>=48 && field==7)d.choices={"12 dB/oct","24 dB/oct"};
                else if(f.choices){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
            }
            else if (i >= dynamicEqSpectralParameterStart) {
                const auto local=i-dynamicEqSpectralParameterStart,slot=local/dynamicEqSpectralParametersPerSlot+1;
                const auto field=local%dynamicEqSpectralParametersPerSlot;
                const auto prefix="effect."+juce::String(slot)+".dynamic_eq.";
                if(field==0){d.id=prefix+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;d.choices={"Ordinary","Spectral"};d.automatable=false;}
                else {const auto band=(field-1)/dynamicEqSpectralParametersPerBand+1;
                    d.id=prefix+"band."+juce::String(band)+"."+f.id;d.name="Effect "+juce::String(slot)+" Band "+juce::String(band)+" "+f.name;
                    if(f.choices){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
                }
            }
            else if (i >= dynamicEqDetectorParameterStart) {
                const auto local=i-dynamicEqDetectorParameterStart;
                const auto slot=local/dynamicEqDetectorParametersPerSlot+1;
                const auto band=(local%dynamicEqDetectorParametersPerSlot)/dynamicEqDetectorParametersPerBand+1;
                d.id="effect."+juce::String(slot)+".dynamic_eq.band."+juce::String(band)+"."+f.id;
                d.name="Effect "+juce::String(slot)+" Band "+juce::String(band)+" "+f.name;
                if(local%dynamicEqDetectorParametersPerBand<2){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
                else if(local%dynamicEqDetectorParametersPerBand==2)d.choices={"Internal band","Internal free","External band","External free"};
            }
            else if (i >= delayStereoParameterStart) {
                const auto slot=(i-delayStereoParameterStart)/delayStereoParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
                if(f.choices==2){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
                else if(f.choices)d.choices={"Beats","1/64 triplet","1/64","1/32 triplet","1/64 dotted","1/32","1/16 triplet","1/32 dotted",
                    "1/16","1/8 triplet","1/16 dotted","1/8","1/4 triplet","1/8 dotted","1/4","1/2 triplet","1/4 dotted","1/2",
                    "1/1 triplet","1/2 dotted","1/1","1/1 dotted"};
            }
            else if (i >= dynamicEqExtensionParameterStart) {
                const auto local=i-dynamicEqExtensionParameterStart;
                const auto slot=local/dynamicEqExtensionParametersPerSlot+1;
                const auto band=(local%dynamicEqExtensionParametersPerSlot)/dynamicEqExtensionParametersPerBand+1;
                d.id="effect."+juce::String(slot)+".dynamic_eq.band."+juce::String(band)+"."+f.id;
                d.name="Effect "+juce::String(slot)+" Band "+juce::String(band)+" "+f.name;
                d.choices=local%dynamicEqExtensionParametersPerBand==0
                    ?std::vector<juce::String>{"Legacy","Band pass","Tilt shelf","Flat tilt"}
                    :std::vector<juce::String>{"Legacy","6 dB/oct","36 dB/oct","48 dB/oct","96 dB/oct"};
            }
            else if (i >= delayTapeParameterStart) {
                const auto slot=(i-delayTapeParameterStart)/delayTapeParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
                if(f.choices){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
            }
            else if (i >= dynamicEqPlacementParameterStart) {
                const auto slot=(i-dynamicEqPlacementParameterStart)/dynamicEqPlacementParametersPerSlot+1;
                const auto band=(i-dynamicEqPlacementParameterStart)%dynamicEqPlacementParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+".dynamic_eq.band."+juce::String(band)+".placement";
                d.name="Effect "+juce::String(slot)+" Band "+juce::String(band)+" Placement";
                d.choices={"Stereo","Left","Right","Mid","Side"};
            }
            else if (i >= dynamicEqGainParameterStart) {
                const auto slot=(i-dynamicEqGainParameterStart)/dynamicEqGainParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+".dynamic_eq."+f.id;
                d.name="Effect "+juce::String(slot)+" "+f.name;
                if(f.choices){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
            }
            else if (i >= dynamicEqParameterStart) {
                const auto slot=(i-dynamicEqParameterStart)/dynamicEqParametersPerSlot+1;
                const auto local=(i-dynamicEqParameterStart)%dynamicEqParametersPerSlot;
                const auto band=local/dynamicEqParametersPerBand+1;
                const auto prefix="effect."+juce::String(slot)+".dynamic_eq.";
                d.id=prefix+(local==88?juce::String(f.id):"band."+juce::String(band)+"."+f.id);
                d.name="Effect "+juce::String(slot)+" "+(local==88?juce::String(f.name):"Band "+juce::String(band)+" "+f.name);
                if(local!=88 && local%dynamicEqParametersPerBand==2)d.choices={"Bell","Low shelf","High shelf","Low cut","High cut","Notch"};
                else if(local!=88 && local%dynamicEqParametersPerBand==6)d.choices={"12 dB/oct","24 dB/oct"};
                else if(f.choices){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
            }
            else if (i >= midSideSplitParameterStart) {
                const auto slot=(i-midSideSplitParameterStart)/midSideSplitParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
                if(f.choices){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
            }
            else if (i >= frequencySplitParameterStart) {
                const auto slot=(i-frequencySplitParameterStart)/frequencySplitParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
                if(f.choices){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
            }
            else if (i >= convolutionParameterStart) {
                const auto slot=(i-convolutionParameterStart)/convolutionParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
                if(f.choices)d.choices={"Unit Impulse","Short Room"};
            }
            else if (i >= frequencyShiftParameterStart) {
                const auto slot=(i-frequencyShiftParameterStart)/frequencyShiftParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
            }
            else if (i >= dimensionParameterStart) {
                const auto slot=(i-dimensionParameterStart)/dimensionParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
            }
            else if (i >= stereoParameterStart) {
                const auto slot=(i-stereoParameterStart)/stereoParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
                if(f.choices)d.choices={"Stereo","Swap","Mono"};
            }
            else if (i >= flangerParameterStart) {
                const auto slot=(i-flangerParameterStart)/flangerParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
            }
            else if (i >= phaserParameterStart) {
                const auto slot=(i-phaserParameterStart)/phaserParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
            }
            else if (i >= chorusParameterStart) {
                const auto slot=(i-chorusParameterStart)/chorusParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
            }
            else if (i >= filterParameterStart) {
                const auto slot=(i-filterParameterStart)/filterParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
                if(f.choices)d.choices={"Low-pass","Band-pass","High-pass"};
            }
            else if (i >= eqParameterStart) {
                const auto slot=(i-eqParameterStart)/eqParametersPerSlot+1;
                d.id="effect."+juce::String(slot)+"."+f.id;d.name="Effect "+juce::String(slot)+" "+f.name;
            }
            else if (i >= 756) {
                d.id="output."+juce::String(f.id);d.name=f.name;
                if(i==757){d.kind=ParameterKind::toggle;d.choices={"Off","On"};}
                if(i==758)d.choices={"Realtime","Higher offline"};
            }
            else if (i >= 212) {
                if(i<220){ d.id="synth."+juce::String((i-212)/2+1)+"."+f.id; d.name="Synth "+juce::String((i-212)/2+1)+" "+f.name; }
                else if(i<252){ d.id="synth."+juce::String((i-220)/8+1)+".lfo."+juce::String((i-220)%8/4+1)+"."+f.id; d.name="Synth "+juce::String((i-220)/8+1)+" LFO "+juce::String((i-220)%8/4+1)+" "+f.name; }
                else if(i<572){ d.id="synth."+juce::String((i-252)/80+1)+".modulation."+juce::String((i-252)%80/5+1)+"."+f.id; d.name="Synth "+juce::String((i-252)/80+1)+" Route "+juce::String((i-252)%80/5+1)+" "+f.name; }
                else { d.id="effect."+juce::String((i-572)/23+1)+"."+f.id;d.name="Effect "+juce::String((i-572)/23+1)+" "+f.name; }
                if (f.choices) {
                    if (i<220 || (i<252&&(i-220)%4==1) || (i>=252&&i<572&&(i-252)%5==0) || (i>=572&&((i-572)%23==0||(i-572)%23==5))) {
                        d.kind=ParameterKind::toggle;d.choices={"Off","On"};
                    } else if(i<252)d.choices={"Sine","Triangle","Saw","Square"};
                    else if(i<572) {
                        switch((i-252)%5){
                            case 1:d.choices={"None","Amp envelope","Filter envelope","LFO 1","LFO 2","Velocity","Note"};break;
                            case 2:d.choices={"None","Level","Pan","Osc 1 level","Osc 2 level","Osc 1 tune","Osc 2 tune","Osc 1 detune","Osc 2 detune","Osc 1 width","Osc 2 width","Osc 1 position","Osc 2 position","Noise","Cutoff","Resonance","Filter envelope amount"};break;
                            case 3:d.choices={"Common","Analog","Wavetable"};break;
                        }
                    } else d.choices={"Tanh","Soft sign"};
                }
            }
            else if (i < bankStart)
            {
                const auto slot = (i - commonStart) / 4 + 1, offset = (i - commonStart) % 4;
                d.id = "synth." + juce::String(slot) + "." + f.id;
                d.name = "Synth " + juce::String(slot) + " " + f.name;
                if (offset == 0)
                {
                    d.kind = ParameterKind::toggle; d.choices = {"Off", "On"};
                    d.defaultValue = slot == 1 ? 1.0f : 0.0f;
                }
                else if (offset == 1) d.choices = {"Analog", "Wavetable"};
            }
            else
            {
                const bool analog = i < wavetableStart;
                const auto slot = (i - (analog ? bankStart : wavetableStart)) / bankSize + 1;
                const auto offset = (i - bankStart) % bankSize;
                d.id = "synth." + juce::String(slot) + (analog ? ".analog." : ".wavetable.");
                d.name = "Synth " + juce::String(slot) + (analog ? " Analog " : " Wavetable ");
                if (offset < 10)
                {
                    const auto osc = offset / 5 + 1;
                    d.id += "osc." + juce::String(osc) + ".";
                    d.name += "Osc " + juce::String(osc) + " ";
                    if (offset % 5 == 0)
                        d.choices = analog ? std::vector<juce::String>{"Sine", "Triangle", "Saw", "Pulse"}
                                           : std::vector<juce::String>{"0", "1"};
                }
                else if (offset == 13) d.choices = {"Lowpass", "Bandpass", "Highpass"};
                d.id += f.id; d.name += f.name;
            }
            d.stateKey = stateKey(d.id);
            numericKeys[i] = {d.stateKey,static_cast<std::uint32_t>(i)};
            if (std::any_of(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(i),
                            [&](const auto& prior) { return prior.stateKey == d.stateKey; }))
                throw std::logic_error("Duplicate sound-state key in fixed parameter registry");
        }
        // Decode resolves every record; avoid scanning the whole registry for each key.
        std::sort(numericKeys.begin(),numericKeys.end());
        return result;
    }();
    return descriptors;
}

ParameterValues defaultParameterValues()
{
    ParameterValues values;
    for (std::size_t i = 0; i < parameterCount; ++i)
    {
        const auto& f = fieldAt(i);
        const float initial=i>=drumParameterStart&&i<drumModeParameterStart&&(i-drumParameterStart)%drumParametersPerPad==7
            ?float(36+((i-drumParameterStart)%drumParametersPerSlot)/drumParametersPerPad)
            :i==commonStart?1.0f:f.initial;
        values[i]=range(f).convertTo0to1(initial);
    }
    return values;
}

static bool normalizedValuesValid(const ParameterValues& values) noexcept
{
    for (std::size_t i = 0; i < parameterCount; ++i)
    {
        const auto v = values[i];
        if (!std::isfinite(v) || v < 0 || v > 1) return false;
        const auto choices = fieldAt(i).choices;
        if (choices != 0)
        {
            const auto steps = static_cast<float>(choices - 1);
            if (std::abs(v - std::round(v * steps) / steps) > 1e-6f) return false;
        }
    }
    for(std::size_t base=dynamicEqDetectorParameterStart;base<dynamicEqSpectralParameterStart;base+=dynamicEqDetectorParametersPerBand){
        const auto source=int(physical(values,base+2));
        if((source==1||source==3)&&physical(values,base+3)>=physical(values,base+4))return false;
    }
    for(std::size_t slot=0;slot<8;++slot){
        const auto spectral=dynamicEqSpectralParameterStart+slot*dynamicEqSpectralParametersPerSlot;
        if(values[spectral]==0)continue;
        for(std::size_t band=0;band<8;++band)if(values[spectral+1+band*3]==1){
            const auto detector=dynamicEqDetectorParameterStart+slot*40+band*5;
            const auto extension=dynamicEqExtensionParameterStart+slot*16+band*2;
            const auto original=dynamicEqParameterStart+slot*89+band*11;
            if(physical(values,original+2)>2 || values[extension]!=0 || values[detector]!=0 || values[detector+1]!=0 || values[detector+2]!=0)return false;
        }
    }
    return true;
}

bool makeSynthPatch(const ParameterValues& values, SynthPatch& output) noexcept
{
    if (!normalizedValuesValid(values)) return false;
    SynthPatch patch;
    patch.masterLevel = values[0];
    patch.performance = {static_cast<VoiceMode>(int(physical(values,9))),physical(values,10),physical(values,11)==1};
    for (std::size_t slot = 0; slot < patch.layers.size(); ++slot)
    {
        auto& layer = patch.layers[slot];
        for(std::size_t k=0;k<2;++k) {
            const auto n=220+slot*8+k*4;
            layer.lfos[k]={static_cast<LfoShape>(int(physical(values,n))),physical(values,n+1)==1,physical(values,n+2),physical(values,n+3)};
        }
        for(std::size_t k=0;k<16;++k) {
            const auto n=252+slot*80+k*5;
            layer.modulation[k]={physical(values,n)==1,static_cast<ModSource>(int(physical(values,n+1))),static_cast<ModTarget>(int(physical(values,n+2))),static_cast<ModBank>(int(physical(values,n+3))),physical(values,n+4)};
        }
        const auto c = commonStart + slot * 4;
        layer.enabled = physical(values, c) == 1;
        layer.engine = static_cast<EngineKind>(static_cast<int>(physical(values, c + 1)));
        layer.level = values[c + 2]; layer.pan = physical(values, c + 3);
        const bool analog = layer.engine == EngineKind::analog;
        const auto b = (analog ? bankStart : wavetableStart) + slot * bankSize;
        for (std::size_t osc = 0; osc < 2; ++osc)
        {
            auto& o = layer.oscillators[osc];
            const auto n = b + osc * 5;
            if (analog)
            {
                o.waveform = static_cast<Waveform>(static_cast<int>(physical(values, n)));
                o.tune = physical(values, n + 1); o.detune = physical(values, n + 2);
                o.level = values[n + 3]; o.pulseWidth = physical(values, n + 4);
            }
            else
            {
                o.tableIndex = static_cast<int>(physical(values, n)); o.position = values[n + 1];
                o.tune = physical(values, n + 2); o.detune = physical(values, n + 3); o.level = values[n + 4];
            }
        }
        layer.noiseLevel = values[b + 10]; layer.cutoffHz = physical(values, b + 11);
        layer.resonance = physical(values, b + 12);
        layer.filterMode = static_cast<FilterMode>(static_cast<int>(physical(values, b + 13)));
        layer.ampEnvelope = {physical(values, b + 14), physical(values, b + 15), physical(values, b + 17), values[b + 16]};
        layer.filterEnvelope = {physical(values, b + 18), physical(values, b + 19), physical(values, b + 21), values[b + 20]};
        layer.filterEnvelopeOctaves = physical(values, b + 22);
        if (!analog)
        {
            const auto w = wavetableWarmthParameterStart + slot * wavetableWarmthParametersPerSlot;
            for (std::size_t osc = 0; osc < 2; ++osc)
            {
                layer.oscillators[osc].unison = 1 + static_cast<int>(physical(values, w + 2 * osc));
                layer.oscillators[osc].unisonCents = physical(values, w + 2 * osc + 1);
            }
            layer.unisonWidth = physical(values, w + 4); layer.drift = physical(values, w + 5);
            layer.filterType = static_cast<FilterType>(static_cast<int>(physical(values, w + 6)));
            layer.vowelSize = physical(values, w + 7); layer.attackCurve = physical(values, w + 8);
        }
    }
    if (!validateSynthPatch(patch)) return false;
    output = patch;
    return true;
}

bool validateParameterValues(const ParameterValues& values) noexcept
{
    SynthPatch patch;
    return makeSynthPatch(values,patch);
}
std::optional<std::size_t> parameterIndexForKey(std::uint32_t key) noexcept
{
    if(key==0)return std::nullopt;
    const auto it=std::lower_bound(numericKeys.begin(),numericKeys.end(),key,
        [](const auto& entry,std::uint32_t value){return entry.first<value;});
    if(it==numericKeys.end()||it->first!=key)return std::nullopt;
    return static_cast<std::size_t>(it->second);
}
bool isContinuousParameter(std::size_t index) noexcept
{ return index<parameterCount && fieldAt(index).choices==0; }
float parameterPhysicalValue(const ParameterValues& values,std::size_t index) noexcept
{ return physical(values,index); }
}
