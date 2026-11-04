#include "RackExamples.h"
#include "ParameterRegistry.h"
#include "RackReconciliation.h"
#include <algorithm>
#include <juce_cryptography/juce_cryptography.h>
namespace airack {
namespace {
void value(RackUnit& u,const juce::String& key,float physical){const auto index=rackParameterIndex(u.module,0,key);jassert(index);const auto& d=parameterRegistry()[*index];const juce::NormalisableRange<float> range(d.minimum,d.maximum,0,d.skew);u.defaults.push_back({u.id,key,range.convertTo0to1(physical)});}
FocusedControl focus(const char* label,int unit,int section,const char* key,FocusWidget widget,float low,float high,FocusResponse response=FocusResponse::linear){return {label,std::uint8_t(unit),std::uint8_t(section),key,widget,response,low,high};}
void route(RackUnit& u,float source,float target,float bank,float amount,int number=1){const auto key=[number](const char* field){return "modulation."+juce::String(number)+"."+field;};value(u,key("enabled"),1);value(u,key("source"),source);value(u,key("target"),target);value(u,key("bank"),bank);value(u,key("amount"),amount);}
}
std::vector<RackDesign> factoryRackDesigns(){
 RackDesign copper;copper.id=juce::Uuid("91387715-13d3-4c57-a479-12251842a402");copper.name="Copper Arc";
 copper.intent="A dry saw and triangle analog starting voice with a rounded filter contour and subtle slow motion. Shape bass, pluck, lead, brass and pad through ordinary native controls.";
 copper.role=RackRole::synth;copper.units={{0,NativeModule::analog,{},2}};auto& a=copper.units[0];
 value(a,"enabled",1);value(a,"engine",0);value(a,"level",.678f);value(a,"pan",0);
 value(a,"analog.osc.1.waveform",2);value(a,"analog.osc.1.level",.46f);value(a,"analog.osc.1.tune",0);value(a,"analog.osc.1.detune",-3);value(a,"analog.osc.1.pulse_width",.46f);
 value(a,"analog.osc.2.waveform",1);value(a,"analog.osc.2.level",.24f);value(a,"analog.osc.2.tune",0);value(a,"analog.osc.2.detune",3);value(a,"analog.osc.2.pulse_width",.54f);
 value(a,"analog.noise",0);value(a,"analog.filter.mode",0);value(a,"analog.filter.cutoff",2200);value(a,"analog.filter.resonance",.22f);
 value(a,"analog.amp.attack",.012f);value(a,"analog.amp.decay",.25f);value(a,"analog.amp.sustain",.72f);value(a,"analog.amp.release",.45f);
 value(a,"analog.filter_env.attack",.02f);value(a,"analog.filter_env.decay",.3f);value(a,"analog.filter_env.sustain",.18f);value(a,"analog.filter_env.release",.4f);value(a,"analog.filter_env.octaves",.6f);
 value(a,"lfo.1.shape",0);value(a,"lfo.1.synced",0);value(a,"lfo.1.rate_hz",.25f);route(a,3,14,1,.035f);
 copper.panel.layout=PanelLayout::instrumentSections;copper.panel.sections={"Tone","Envelope","Motion"};
 copper.panel.controls={focus("Brightness",0,0,"analog.filter.cutoff",FocusWidget::rotary,20,14000,FocusResponse::logarithmic),focus("Resonance",0,0,"analog.filter.resonance",FocusWidget::rotary,0,.85f),focus("Body",0,0,"analog.osc.2.level",FocusWidget::rotary,0,.65f),focus("Attack",0,1,"analog.amp.attack",FocusWidget::verticalSlider,.001f,2,FocusResponse::logarithmic),focus("Release",0,1,"analog.amp.release",FocusWidget::verticalSlider,.02f,3,FocusResponse::logarithmic),focus("Contour",0,1,"analog.filter_env.decay",FocusWidget::verticalSlider,.02f,2,FocusResponse::logarithmic),focus("Motion rate",0,2,"lfo.1.rate_hz",FocusWidget::rotary,.05f,5,FocusResponse::logarithmic),focus("Motion",0,2,"modulation.1.amount",FocusWidget::rotary,-.25f,.25f)};
 RackDesign prism;prism.id=juce::Uuid("91387715-13d3-4c57-a479-12251842b402");prism.name="Prism Relay";
 prism.intent="A dry focused digital voice with contrasting saw and square spectral morphs, a quick contour and restrained spectral motion. Shape bright bass, pluck, dense lead, moving chord and airy pad through ordinary native controls.";
 prism.role=RackRole::synth;prism.units={{0,NativeModule::wavetable,{},2}};auto& w=prism.units[0];
 value(w,"enabled",1);value(w,"engine",1);value(w,"level",.405f);value(w,"pan",0);
 value(w,"wavetable.osc.1.table",0);value(w,"wavetable.osc.1.position",.62f);value(w,"wavetable.osc.1.level",.48f);value(w,"wavetable.osc.1.tune",0);value(w,"wavetable.osc.1.detune",-4);
 value(w,"wavetable.osc.2.table",1);value(w,"wavetable.osc.2.position",.36f);value(w,"wavetable.osc.2.level",.22f);value(w,"wavetable.osc.2.tune",0);value(w,"wavetable.osc.2.detune",4);
 value(w,"wavetable.noise",0);value(w,"wavetable.filter.mode",0);value(w,"wavetable.filter.cutoff",7200);value(w,"wavetable.filter.resonance",.16f);
 value(w,"wavetable.amp.attack",.008f);value(w,"wavetable.amp.decay",.18f);value(w,"wavetable.amp.sustain",.68f);value(w,"wavetable.amp.release",.32f);
 value(w,"wavetable.filter_env.attack",.006f);value(w,"wavetable.filter_env.decay",.22f);value(w,"wavetable.filter_env.sustain",.14f);value(w,"wavetable.filter_env.release",.24f);value(w,"wavetable.filter_env.octaves",.4f);
 value(w,"lfo.1.shape",0);value(w,"lfo.1.synced",0);value(w,"lfo.1.rate_hz",.35f);route(w,3,11,2,.035f);
 prism.panel.layout=PanelLayout::instrumentSections;prism.panel.sections={"Spectrum","Envelope","Motion"};
 prism.panel.controls={focus("Color",0,0,"wavetable.osc.1.position",FocusWidget::rotary,0,1),focus("Density",0,0,"wavetable.osc.2.level",FocusWidget::rotary,0,.65f),focus("Brightness",0,0,"wavetable.filter.cutoff",FocusWidget::rotary,20,18000,FocusResponse::logarithmic),focus("Resonance",0,0,"wavetable.filter.resonance",FocusWidget::rotary,0,.85f),focus("Attack",0,1,"wavetable.amp.attack",FocusWidget::verticalSlider,.001f,2,FocusResponse::logarithmic),focus("Release",0,1,"wavetable.amp.release",FocusWidget::verticalSlider,.02f,3,FocusResponse::logarithmic),focus("Motion rate",0,2,"lfo.1.rate_hz",FocusWidget::rotary,.05f,5,FocusResponse::logarithmic),focus("Motion",0,2,"modulation.1.amount",FocusWidget::rotary,-.25f,.25f)};
 RackDesign lumen;lumen.id=juce::Uuid("91387715-13d3-4c57-a479-12251842c401");lumen.name="Lumen Drift";
 lumen.intent="A diffused, slowly modulated reverb starting space. Shape size, decay, damping, predelay and blend through the five native controls.";
 lumen.role=RackRole::fx;lumen.units={{0,NativeModule::reverb,{},2}};auto& r=lumen.units[0];
 value(r,"bypassed",0);value(r,"reverb.size",.9f);value(r,"reverb.decay_seconds",4);value(r,"reverb.damping",.2f);value(r,"reverb.predelay_ms",20);value(r,"reverb.mix",.25f);
 lumen.panel.layout=PanelLayout::orderedFxStrip;lumen.panel.sections={"Space","Tone","Blend"};
 lumen.panel.controls={focus("Size",0,0,"reverb.size",FocusWidget::rotary,0,1),focus("Decay",0,0,"reverb.decay_seconds",FocusWidget::rotary,.1f,20,FocusResponse::logarithmic),focus("Predelay",0,0,"reverb.predelay_ms",FocusWidget::rotary,0,250),focus("Damping",0,1,"reverb.damping",FocusWidget::rotary,0,1),focus("Mix",0,2,"reverb.mix",FocusWidget::rotary,0,1)};
 RackDesign anchor;anchor.id=juce::Uuid("91387715-13d3-4c57-a479-12251842d402");anchor.name="Anchor Press";
 anchor.intent="A linked peak compressor starting point with a measured gentle grip on the untouched Copper Arc voice. Adapt threshold, timing and blend to the actual input; makeup starts at unity.";
 anchor.role=RackRole::fx;anchor.units={{0,NativeModule::compressor,{},2}};auto& c=anchor.units[0];
 value(c,"bypassed",0);value(c,"compressor.threshold_db",-50.25f);value(c,"compressor.ratio",3);value(c,"compressor.attack_ms",15);value(c,"compressor.release_ms",150);value(c,"compressor.makeup_db",0);value(c,"compressor.mix",1);
 anchor.panel.layout=PanelLayout::orderedFxStrip;anchor.panel.sections={"Grip","Timing","Blend"};
 anchor.panel.controls={focus("Threshold",0,0,"compressor.threshold_db",FocusWidget::rotary,-60,0),focus("Ratio",0,0,"compressor.ratio",FocusWidget::rotary,1,20),focus("Attack",0,1,"compressor.attack_ms",FocusWidget::rotary,.1f,100,FocusResponse::logarithmic),focus("Release",0,1,"compressor.release_ms",FocusWidget::rotary,10,2000,FocusResponse::logarithmic),focus("Makeup",0,2,"compressor.makeup_db",FocusWidget::rotary,0,24),focus("Mix",0,2,"compressor.mix",FocusWidget::rotary,0,1)};
 RackDesign cinder;cinder.id=juce::Uuid("91387715-13d3-4c57-a479-12251842e402");cinder.name="Cinder Coil";
 cinder.intent="A restrained tanh saturation starting point with a softened wet tone and parallel blend. Shape bass body or lead edge through ordinary drive, shape, tone and mix controls.";
 cinder.role=RackRole::fx;cinder.units={{0,NativeModule::drive,{},2}};auto& d=cinder.units[0];
 value(d,"bypassed",0);value(d,"drive.gain_db",6);value(d,"drive.shape",0);value(d,"drive.tone_hz",6500);value(d,"drive.mix",.25f);
 cinder.panel.layout=PanelLayout::orderedFxStrip;cinder.panel.sections={"Drive","Tone","Blend"};
 cinder.panel.controls={focus("Drive",0,0,"drive.gain_db",FocusWidget::rotary,0,24),focus("Shape",0,0,"drive.shape",FocusWidget::choice,0,1),focus("Tone",0,1,"drive.tone_hz",FocusWidget::rotary,200,20000,FocusResponse::logarithmic),focus("Mix",0,2,"drive.mix",FocusWidget::rotary,0,1)};
 RackDesign echo;echo.id=juce::Uuid("91387715-13d3-4c57-a479-12251842f402");echo.name="Echo Thread";
 echo.intent="A quiet quarter-second free echo with filtered repeats and restrained stereo crossfeed. Switch to host beats or shape short and longer echoes through the existing timing and feedback controls.";
 echo.role=RackRole::fx;echo.units={{0,NativeModule::delay,{},2}};auto& t=echo.units[0];
 value(t,"bypassed",0);value(t,"delay.synced",0);value(t,"delay.seconds",.25f);value(t,"delay.beats",.5f);value(t,"delay.feedback",.28f);value(t,"delay.stereo",.35f);value(t,"delay.filter_hz",4500);value(t,"delay.mix",.18f);
 echo.panel.layout=PanelLayout::orderedFxStrip;echo.panel.sections={"Time","Repeats","Blend"};
 echo.panel.controls={focus("Sync",0,0,"delay.synced",FocusWidget::toggle,0,1),focus("Time",0,0,"delay.seconds",FocusWidget::rotary,.001f,4,FocusResponse::logarithmic),focus("Beats",0,0,"delay.beats",FocusWidget::rotary,.0625f,1,FocusResponse::logarithmic),focus("Feedback",0,1,"delay.feedback",FocusWidget::rotary,0,.95f),focus("Stereo",0,1,"delay.stereo",FocusWidget::rotary,0,1),focus("Filter",0,1,"delay.filter_hz",FocusWidget::rotary,20,20000,FocusResponse::logarithmic),focus("Mix",0,2,"delay.mix",FocusWidget::rotary,0,1)};
 RackDesign contour;contour.id=juce::Uuid("91387715-13d3-4c57-a479-12251842a802");contour.name="Contour Loom";
 contour.intent="A transparent three-band EQ starting point. Shape broad low and high shelves or focus the bell through all nine ordinary native controls.";
 contour.role=RackRole::fx;contour.units={{0,NativeModule::eq,{},2}};auto& q=contour.units[0];
 value(q,"bypassed",0);value(q,"eq.low_shelf.frequency_hz",160);value(q,"eq.low_shelf.gain_db",0);value(q,"eq.low_shelf.q",.70710678f);
 value(q,"eq.bell.frequency_hz",1000);value(q,"eq.bell.gain_db",0);value(q,"eq.bell.q",1);
 value(q,"eq.high_shelf.frequency_hz",8000);value(q,"eq.high_shelf.gain_db",0);value(q,"eq.high_shelf.q",.70710678f);
 contour.panel.layout=PanelLayout::orderedFxStrip;contour.panel.sections={"Low shelf","Bell","High shelf"};
 contour.panel.controls={focus("Low frequency",0,0,"eq.low_shelf.frequency_hz",FocusWidget::rotary,20,2000,FocusResponse::logarithmic),focus("Low gain",0,0,"eq.low_shelf.gain_db",FocusWidget::rotary,-18,18),focus("Low Q",0,0,"eq.low_shelf.q",FocusWidget::rotary,.25f,4),focus("Bell frequency",0,1,"eq.bell.frequency_hz",FocusWidget::rotary,20,20000,FocusResponse::logarithmic),focus("Bell gain",0,1,"eq.bell.gain_db",FocusWidget::rotary,-18,18),focus("Bell Q",0,1,"eq.bell.q",FocusWidget::rotary,.25f,4),focus("High frequency",0,2,"eq.high_shelf.frequency_hz",FocusWidget::rotary,1000,20000,FocusResponse::logarithmic),focus("High gain",0,2,"eq.high_shelf.gain_db",FocusWidget::rotary,-18,18)};
 RackDesign veil;veil.id=juce::Uuid("91387715-13d3-4c57-a479-12251842b802");veil.name="Veil Current";
 veil.intent="A low-pass tonal insert with neutral input drive. Choose normalized band-pass or high-pass, adjust resonance, and blend memoryless saturation through ordinary native controls.";
 veil.role=RackRole::fx;veil.units={{0,NativeModule::filter,{},2}};auto& f=veil.units[0];
 value(f,"bypassed",0);value(f,"filter_fx.mode",0);value(f,"filter_fx.cutoff_hz",4000);value(f,"filter_fx.resonance",.70710678f);value(f,"filter_fx.drive_db",0);value(f,"filter_fx.mix",1);
 veil.panel.layout=PanelLayout::orderedFxStrip;veil.panel.sections={"Filter","Drive"};
 veil.panel.controls={focus("Mode",0,0,"filter_fx.mode",FocusWidget::choice,0,2),focus("Cutoff",0,0,"filter_fx.cutoff_hz",FocusWidget::rotary,20,20000,FocusResponse::logarithmic),focus("Resonance",0,0,"filter_fx.resonance",FocusWidget::rotary,.5f,10),focus("Drive",0,1,"filter_fx.drive_db",FocusWidget::rotary,0,24),focus("Mix",0,1,"filter_fx.mix",FocusWidget::rotary,0,1)};
 RackDesign orbit;orbit.id=juce::Uuid("91387715-13d3-4c57-a479-12251842c802");orbit.revision=1;orbit.name="Orbit Ribbon";
 orbit.intent="Slow dual-mono delay motion with a light feedback ribbon.";orbit.role=RackRole::fx;orbit.units={{0,NativeModule::chorus,{},2}};auto& ch=orbit.units[0];
 value(ch,"bypassed",0);value(ch,"chorus.rate_hz",.35f);value(ch,"chorus.depth",.25f);value(ch,"chorus.base_delay_ms",18);value(ch,"chorus.feedback",.1f);value(ch,"chorus.mix",.25f);
 orbit.panel.layout=PanelLayout::orderedFxStrip;orbit.panel.sections={"Motion","Blend"};orbit.panel.surface=1;orbit.panel.accent=1;
 orbit.panel.controls={focus("Rate",0,0,"chorus.rate_hz",FocusWidget::rotary,.1f,5),focus("Depth",0,0,"chorus.depth",FocusWidget::rotary,0,1),focus("Delay",0,0,"chorus.base_delay_ms",FocusWidget::rotary,12,40),focus("Feedback",0,1,"chorus.feedback",FocusWidget::rotary,-.8f,.8f),focus("Mix",0,1,"chorus.mix",FocusWidget::rotary,0,1)};
 RackDesign aster;aster.id=juce::Uuid("91387715-13d3-4c57-a479-12251842d901");aster.revision=1;aster.name="Aster Sweep";
 aster.intent="Slow dual-mono allpass motion with a blended notch sweep and restrained signed feedback.";aster.role=RackRole::fx;aster.units={{0,NativeModule::phaser,{}}};auto& ph=aster.units[0];
 value(ph,"bypassed",0);value(ph,"phaser.rate_hz",.35f);value(ph,"phaser.depth",.6f);value(ph,"phaser.centre_hz",1300);value(ph,"phaser.feedback",.2f);value(ph,"phaser.mix",.5f);
 aster.panel.layout=PanelLayout::orderedFxStrip;aster.panel.sections={"Motion","Blend"};aster.panel.surface=1;aster.panel.accent=1;
 aster.panel.controls={focus("Rate",0,0,"phaser.rate_hz",FocusWidget::rotary,.1f,5),focus("Depth",0,0,"phaser.depth",FocusWidget::rotary,0,1),focus("Centre",0,0,"phaser.centre_hz",FocusWidget::rotary,20,20000,FocusResponse::logarithmic),focus("Feedback",0,1,"phaser.feedback",FocusWidget::rotary,-.8f,.8f),focus("Mix",0,1,"phaser.mix",FocusWidget::rotary,0,1)};
 RackDesign silver;silver.id=juce::Uuid("91387715-13d3-4c57-a479-12251842e902");silver.revision=1;silver.name="Silver Loop";
 silver.intent="Dual-mono short-delay comb motion with restrained signed feedback and an even wet/dry blend.";silver.role=RackRole::fx;silver.units={{0,NativeModule::flanger,{},2}};auto& fl=silver.units[0];
 value(fl,"bypassed",0);value(fl,"flanger.rate_hz",.25f);value(fl,"flanger.depth",.6f);value(fl,"flanger.base_delay_ms",2);value(fl,"flanger.feedback",.35f);value(fl,"flanger.mix",.5f);
 silver.panel.layout=PanelLayout::orderedFxStrip;silver.panel.sections={"Motion","Blend"};silver.panel.surface=1;silver.panel.accent=1;
 silver.panel.controls={focus("Rate",0,0,"flanger.rate_hz",FocusWidget::rotary,.05f,5),focus("Depth",0,0,"flanger.depth",FocusWidget::rotary,0,1),focus("Delay",0,0,"flanger.base_delay_ms",FocusWidget::rotary,1,8),focus("Feedback",0,1,"flanger.feedback",FocusWidget::rotary,-.8f,.8f),focus("Mix",0,1,"flanger.mix",FocusWidget::rotary,0,1)};
 RackDesign centre;centre.id=juce::Uuid("91387715-13d3-4c57-a479-12251842f901");centre.revision=1;centre.name="Centre Line";
 centre.intent="Neutral stereo trim, unity-centre balance and existing-side width with explicit Stereo, Swap or Mono channels.";centre.role=RackRole::fx;centre.units={{0,NativeModule::stereo,{}}};auto& st=centre.units[0];
 value(st,"bypassed",0);value(st,"stereo.gain_db",0);value(st,"stereo.pan",0);value(st,"stereo.width",1);value(st,"stereo.channel_mode",0);
 centre.panel.layout=PanelLayout::orderedFxStrip;centre.panel.sections={"Level","Channels"};centre.panel.surface=1;centre.panel.accent=1;
 centre.panel.controls={focus("Gain",0,0,"stereo.gain_db",FocusWidget::rotary,-24,12),focus("Balance",0,0,"stereo.pan",FocusWidget::rotary,-1,1),focus("Width",0,1,"stereo.width",FocusWidget::rotary,0,2),focus("Channels",0,1,"stereo.channel_mode",FocusWidget::choice,0,2)};
 RackDesign twin;twin.id=juce::Uuid("91387715-13d3-4c57-a479-12251842aa02");twin.revision=1;twin.name="Twin Current";
 twin.intent="Opposed moving taps add calibrated generated side to the original stereo image while retaining its mono fold.";twin.role=RackRole::fx;twin.units={{0,NativeModule::dimension,{},2}};auto& di=twin.units[0];
 value(di,"bypassed",0);value(di,"dimension.depth",.6f);value(di,"dimension.rate_hz",.35f);value(di,"dimension.width",1);value(di,"dimension.mix",.5f);
 twin.panel.layout=PanelLayout::orderedFxStrip;twin.panel.sections={"Motion","Width"};twin.panel.surface=1;twin.panel.accent=1;
 twin.panel.controls={focus("Depth",0,0,"dimension.depth",FocusWidget::rotary,0,1),focus("Rate",0,0,"dimension.rate_hz",FocusWidget::rotary,.05f,2),focus("Width",0,1,"dimension.width",FocusWidget::rotary,0,2),focus("Mix",0,1,"dimension.mix",FocusWidget::rotary,0,1)};
 RackDesign sideband;sideband.id=juce::Uuid("91387715-13d3-4c57-a479-12251842bb01");sideband.revision=1;sideband.name="Sideband Tide";
 sideband.intent="A restrained upward single-sideband translation. Symmetric stereo offset gives independent channel shifts; zero shift retains quadrature colour.";sideband.role=RackRole::fx;sideband.units={{0,NativeModule::frequencyShift,{}}};auto& fs=sideband.units[0];
 value(fs,"bypassed",0);value(fs,"frequency_shift.hz",7);value(fs,"frequency_shift.stereo_offset_hz",0);value(fs,"frequency_shift.mix",.25f);
 sideband.panel.layout=PanelLayout::orderedFxStrip;sideband.panel.sections={"Translation","Blend"};sideband.panel.surface=1;sideband.panel.accent=1;
 sideband.panel.controls={focus("Shift",0,0,"frequency_shift.hz",FocusWidget::rotary,-1000,1000),focus("Stereo offset",0,0,"frequency_shift.stereo_offset_hz",FocusWidget::rotary,-100,100),focus("Mix",0,1,"frequency_shift.mix",FocusWidget::rotary,0,1)};
 RackDesign ember;ember.id=juce::Uuid("91387715-13d3-4c57-a479-12251842bc01");ember.revision=1;ember.name="Ember Room";
 ember.intent="The original short stereo room adds a restrained diagonal reflection field; Unit Impulse is an exact-rate reference choice.";ember.role=RackRole::fx;ember.units={{0,NativeModule::convolution,{}}};auto& cv=ember.units[0];
 value(cv,"bypassed",0);value(cv,"convolution.ir",1);value(cv,"convolution.predelay_ms",0);value(cv,"convolution.wet_trim_db",0);value(cv,"convolution.mix",.25f);
 ember.panel.layout=PanelLayout::orderedFxStrip;ember.panel.sections={"Room","Blend"};ember.panel.surface=1;ember.panel.accent=1;
 ember.panel.controls={focus("IR",0,0,"convolution.ir",FocusWidget::choice,0,1),focus("Predelay",0,0,"convolution.predelay_ms",FocusWidget::rotary,0,250),focus("Wet trim",0,1,"convolution.wet_trim_db",FocusWidget::rotary,-24,12),focus("Mix",0,1,"convolution.mix",FocusWidget::rotary,0,1)};
 RackDesign balance;balance.id=juce::Uuid("91387715-13d3-4c57-a479-12251842bd02");balance.revision=1;balance.name="Band Balance";
 balance.intent="Transparent complementary bands with independent gain, mute and spectral tone; ordinary low/high edits reveal their useful balance.";balance.role=RackRole::fx;balance.units={{0,NativeModule::frequencySplit,{},2}};auto& sp=balance.units[0];
 value(sp,"bypassed",0);value(sp,"frequency_split.crossover_hz",1000);value(sp,"frequency_split.low_gain_db",0);value(sp,"frequency_split.low_mute",0);value(sp,"frequency_split.low_tone",0);value(sp,"frequency_split.high_gain_db",0);value(sp,"frequency_split.high_mute",0);value(sp,"frequency_split.high_tone",0);value(sp,"frequency_split.mix",1);
 balance.panel.layout=PanelLayout::orderedFxStrip;balance.panel.sections={"Bands","Balance"};balance.panel.surface=1;balance.panel.accent=1;
 balance.panel.controls={focus("Crossover",0,0,"frequency_split.crossover_hz",FocusWidget::rotary,20,20000),focus("Low gain",0,0,"frequency_split.low_gain_db",FocusWidget::rotary,-24,12),focus("Low mute",0,0,"frequency_split.low_mute",FocusWidget::toggle,0,1),focus("Low tone",0,0,"frequency_split.low_tone",FocusWidget::rotary,-1,1),focus("High gain",0,1,"frequency_split.high_gain_db",FocusWidget::rotary,-24,12),focus("High mute",0,1,"frequency_split.high_mute",FocusWidget::toggle,0,1),focus("High tone",0,1,"frequency_split.high_tone",FocusWidget::rotary,-1,1),focus("Mix",0,1,"frequency_split.mix",FocusWidget::rotary,0,1)};
 RackDesign field;field.id=juce::Uuid("91387715-13d3-4c57-a479-12251842be01");field.revision=1;field.name="Field Balance";
 field.intent="Transparent orthonormal Mid and existing Side with independent gain, mute and spectral tone; ordinary edits focus the centre or balance the stereo field.";field.role=RackRole::fx;field.units={{0,NativeModule::midSideSplit,{}}};auto& ms=field.units[0];
 value(ms,"bypassed",0);value(ms,"mid_side_split.mid_gain_db",0);value(ms,"mid_side_split.mid_mute",0);value(ms,"mid_side_split.mid_tone",0);value(ms,"mid_side_split.side_gain_db",0);value(ms,"mid_side_split.side_mute",0);value(ms,"mid_side_split.side_tone",0);value(ms,"mid_side_split.mix",1);
 field.panel.layout=PanelLayout::orderedFxStrip;field.panel.sections={"Mid","Side"};field.panel.surface=1;field.panel.accent=1;
 field.panel.controls={focus("Mid gain",0,0,"mid_side_split.mid_gain_db",FocusWidget::rotary,-24,12),focus("Mid mute",0,0,"mid_side_split.mid_mute",FocusWidget::toggle,0,1),focus("Mid tone",0,0,"mid_side_split.mid_tone",FocusWidget::rotary,-1,1),focus("Side gain",0,1,"mid_side_split.side_gain_db",FocusWidget::rotary,-24,12),focus("Side mute",0,1,"mid_side_split.side_mute",FocusWidget::toggle,0,1),focus("Side tone",0,1,"mid_side_split.side_tone",FocusWidget::rotary,-1,1),focus("Mix",0,1,"mid_side_split.mix",FocusWidget::rotary,0,1)};

 return {std::move(copper),std::move(prism),std::move(lumen),std::move(anchor),std::move(cinder),std::move(echo),std::move(contour),std::move(veil),std::move(orbit),std::move(aster),std::move(silver),std::move(centre),std::move(twin),std::move(sideband),std::move(ember),std::move(balance),std::move(field)};
}
std::vector<RackDesign> libraryRackDesigns(){
 return factoryRackDesigns();
}

std::optional<RackPackageRef> legacyRackPackageRef(const RackDesign& design){
 struct Identity {const char* uuid;std::uint64_t revision;const char* canonical;const char* id;const char* sha;};
 static constexpr Identity known[]{
  {"c83dab6a-1c4e-48c1-83c4-80d9212ca601",1,"1b61f57c3fb42da21cfc53c39bd309ce58cb6017415f556add7d72b42cf4911a","ember-reed","14b4f5f907054a35a9d6f95513cc1f5ae57b6ad4e4a383b2975bd17700d8f328"},
  {"c83dab6a-1c4e-48c1-83c4-80d9212ca602",1,"8b1afcfbcef817e372928a9d8ff80ef53b8731043b77096968e2dd966dcfe8f4","glass-current","de4271ed345bad7614652855bcce0d16210f34453a10d3645d957d1b020a8af0"},
  {"c83dab6a-1c4e-48c1-83c4-80d9212ca603",1,"cff62f76f8baec2a7f8d153a953f55b1ce69ba6fab0432d99577ddaeac6569ed","thread-press","e6d72527612f86d102ad761a6f60be5417dc8d5f5ac493bbe5a181e0cd47e656"},
  {"330cb53b-cd9b-443c-94d8-2700bc8b88f7",1,"61fb7252e7e804e9742fee81ee7befa5f5778e3bac43c344dca85609037952c0","oxide-return","5a5081b0d1e6a2f013b28744af4c79fe1f08b11a4d8564f455145536c01401d0"},
  {"22d80511-2e4f-4a41-b1dd-203eabbcf32e",1,"82154ffc00dbe85e4af0d2041da99b1f52a3b9888bea29f16b4bd2d2bfe61c69","small-hours","e16f62fcd6c76f0a97568db2f7aeaac9b88e207ba060338c9a6cf3658b8eb764"},
  {"cdeafe3f-9c29-4c7c-9013-8284b505af10",1,"55eb1f05ca9ba938034ede012941c197498a87e89c0af5fa52ef29b936064054","night-dial","7997493ffae0559b3ab57ade97b3bf81ccc560924d5f89c8b24cf09f9cb4e853"},
  {"f97641c2-cd64-46a9-a3ab-0cc027a5f8d4",1,"dd207988425c8356961fbc26a065034d6d49c2cf1709bd05a08671bd76b8c5a8","tile-array","f9aebab44fcaecec16276e1cb7c11f7a422bf73d06c1213ef1d497d076bb25e4"},
  {"72b0ea6d-4c2d-4d93-a3d8-7a904b050001",1,"629c009553850112c958a236a0facedd3d99b6eaa5ae5f47e4caa9c2569a0b6e","contour-dynamics","d5acce992ab4e96b5d33feea3280b4585ebc90d68beb5d26cf8ec77fbf586b30"},
  {"cc73c36a-696e-4b09-9cf3-dcd4dfac16f0",1,"f5ee9cc529ec5a1e7092f645f9bf3a62f49a5e4cf3816d10014f230dd60f2191","lumen-field","5d24c141a237a89030587ba8c9f918461511f8ee04a2e2e1973618f1b5f22f40"},
  {"91387715-13d3-4c57-a479-12251842bf01",2,"84303af35953521165929e3455a2e3f3cfe7d8cda0abf3761462b4d6bf409874","character-drive","9a718b828bd40b2d26912e38fdb1fa55faeaa11860a79a84d398469efa46425a"},
  {"91387715-13d3-4c57-a479-12251842bf02",2,"236d6a5ce909e3385fc0e458979b05cd373246fb67f05174c97a7a890f10bdaf","band-color","00ac0e796dd620a542c8a839c9e83ef4d43901b3561ad8e0ee7461a64846e818"},
 };
 juce::MemoryBlock bytes;if(!encodeRackDesign(design,bytes,RackValidationMode::instanceRecall))return {};
 const auto hash=juce::SHA256(bytes).toHexString();std::optional<RackPackageRef> result;
 for(const auto& row:known)if(design.id==juce::Uuid(row.uuid)&&design.revision==row.revision&&hash==row.canonical){if(result)return {};result=RackPackageRef{row.id,"1.0.0",row.sha};}
 return result;
}
SoundDocument newInstanceSound(){
 SoundDocument empty;empty.compatibility=newInstanceKernelCompatibility();
 empty.layout.synthUse.fill(SlotUse::neverUsed);empty.layout.synthOrder.fill(noSlot);empty.layout.synthCount=0;
 return empty;
}
SoundDocument copperArcSound(){
 SoundDocument blank;blank.compatibility=newInstanceKernelCompatibility();
 RackOperation op;op.design=factoryRackDesigns()[0];op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a4f1");
 op.destination={RackRole::synth,masterChain,0};op.allocation={{0,0}};op.allowSlotReuse=true;
 SoundDocument placed;RackDifference difference;
 [[maybe_unused]]const auto status=makeRackCandidate(blank,op,SoundEditOrigin::manual,placed,difference);
 jassert(status==RackCandidateStatus::ready);
 return placed;
}
}
