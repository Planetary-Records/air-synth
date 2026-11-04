#include "AllControlsPanel.h"

#include "InstrumentProcessor.h"
#include "FxMonitor.h"
#include "UserReach.h"
#include "ProgrammerValues.h"
#include "MakerFace.h"
#include "SaturationGraph.h"

#include <algorithm>
#include <bitset>
#include <cmath>

namespace airack::studio
{
static float num (const juce::var& v) { return (float) (double) v; }

// The PROGRAMMER's body moved up under the keys, so that row 0's divider starts 128 px down; its left as the
// PROGRAMMER's, its right as far from the panel's right edge as the PROGRAMMER's is from the plate's.
static constexpr float rise = 119.0f, bodyLeft = 93.0f, bodyInset = 95.0f;
// The keys as the PROGRAMMER's sub-page keys, the look drawing a face 30 px down each; the value line at their legend.
static constexpr int keyX = 93, keyPitch = 100, keyY = 16, keyWidth = 84, keyHeight = 96, lineY = 93, lineHeight = 24;
static const juce::Colour markColour (0xf2ffffff);

// The approved drawer drawing is 1040 px wide; vertical spacing stays compact
// when a wide editor gives the display more horizontal room.
static Rect detectorArea (int width, float x, float y, float w, float h)
{
    constexpr float vertical = 1485.f / 1040.f;
    const float horizontal = float(width) / 1040.f;
    return { (x - 20) * horizontal, y * vertical, w * horizontal, h * vertical };
}


/** One page of a view: its knobs in rows of engraved sections under their titles, as the PROGRAMMER's pages are drawn,
    across the panel's width. */
class AllControlsPanel::View final : public juce::Component
{
public:
    struct Section
    {
        String title;
        std::vector<std::size_t> knobs;
    };

    View (AllControlsPanel& p, std::vector<std::vector<Section>> r, bool eq, bool sat) : panel (p), rows (std::move (r)), compact (eq), saturation (sat)
    {
        setInterceptsMouseClicks (false, true);
        if(compact&&!rows.empty()&&!rows.front().empty()&&!rows.front().front().knobs.empty()){
            const auto first=rows.front().front().knobs.front();
            if(first>=dynamicEqSpectralParameterStart && first<spaceReverbParameterStart && rows.front().front().knobs.size()==3)
                rows.front().front().knobs.push_back(dynamicEqSpectralParameterStart+(first-dynamicEqSpectralParameterStart)/25*25);
        }
        for (const auto& row : rows)
            for (const auto& s : row)
                for (const auto i : s.knobs)
                {
                    if(compact && i>=dynamicEqExtensionParameterStart && i<delayStereoParameterStart)continue;
                    auto& k = *knobs.emplace_back (std::make_unique<ProgrammerKnob> (panel.processor, panel.look, panel.host.house, i));
                    if(compact && i>=dynamicEqParameterStart && i<dynamicEqGainParameterStart){
                        const auto local=(i-dynamicEqParameterStart)%dynamicEqParametersPerSlot,field=local%dynamicEqParametersPerBand;
                        if(field==2||field==6)k.useEqSelector(dynamicEqExtensionParameterStart+(i-dynamicEqParameterStart)/dynamicEqParametersPerSlot*dynamicEqExtensionParametersPerSlot
                            +local/dynamicEqParametersPerBand*dynamicEqExtensionParametersPerBand+(field==6?1:0));
                    }
                    k.setExplicitFocusOrder ((int) knobs.size());
                    addAndMakeVisible (k);
                    addAndMakeVisible (k.label);
                    addChildComponent (k.entry);
                }
        if(detectorView()){
            const char* ids[]{"eq.detector.live.threshold","eq.detector.live.attack","eq.detector.live.release","eq.detector.status"};
            const char* titles[]{"Live detector threshold","Live detector attack","Live detector release","Detector status"};
            for(std::size_t n=0;n<4;++n){auto& label=*(detectorLabels[n]=std::make_unique<juce::Label>());label.setComponentID(ids[n]);label.setTitle(titles[n]);
                label.setFont(n==3?panel.look.caps.fontForCap(14):panel.look.mono.fontForCap(22));label.setJustificationType(n==3?juce::Justification::centredLeft:juce::Justification::centred);
                label.setColour(juce::Label::textColourId,juce::Colour(0xffcde4d8));label.setInterceptsMouseClicks(false,false);label.setText("--",juce::dontSendNotification);addAndMakeVisible(label);}
        }
        if(spectralView()){
            const char* ids[]{"eq.spectral.eligibility","eq.spectral.effective","eq.spectral.pending"};
            for(std::size_t n=0;n<3;++n){auto& label=*(spectralLabels[n]=std::make_unique<juce::Label>());
                label.setComponentID(ids[n]);label.setTitle(ids[n]);label.setFont(panel.look.caps.fontForCap(n==1?14.f:12.f));
                label.setJustificationType(juce::Justification::centred);label.setColour(juce::Label::textColourId,juce::Colour(0xffcde4d8));
                label.setInterceptsMouseClicks(false,false);addAndMakeVisible(label);
            }
        }
    }

    void layout()
    {
        if (compact) return layoutEq();
        if (saturation) return layoutSaturation();
        const auto& P = panel.model.programmer;
        const auto left = bodyLeft, right = (float) getWidth() - bodyInset;
        lines.clear();
        titles = {};
        std::size_t k = 0;
        for (std::size_t r = 0; r < rows.size() && r < 2; ++r)
        {
            const auto row = P["body"]["rows"][(int) r];
            std::vector<int> counts;
            juce::StringArray names;
            for (const auto& s : rows[r])
            {
                counts.push_back ((int) s.knobs.size());
                names.add (s.title);
            }
            const auto placed = layoutRow (P, counts, left, right);
            for (std::size_t si = 0; si < rows[r].size(); ++si)
                for (std::size_t ki = 0; ki < rows[r][si].knobs.size(); ++ki)
                    knobs[k++]->place ({ placed.centres[si][ki], num (row["knob_y"]) - rise });
            for (const auto d : placed.dividers)
                lines.push_back ({ Rect::leftTopRightBottom (d, num (row["divider"][0]) - rise, d + 1.0f, num (row["divider"][1]) - rise), true });
            const auto laid = layoutTitles (P, panel.look.faces, names, placed.dividers, left, right + 1.0f, num (row["title_baseline"]) - rise,
                                            num (row["rule_y"]) - rise);
            titles.titles.insert (titles.titles.end(), laid.titles.begin(), laid.titles.end());
            titles.lines.insert (titles.lines.end(), laid.lines.begin(), laid.lines.end());
        }
        if (rows.size() > 1)
        {
            const auto y = num (P["body"]["separator"]["y"]) - rise;
            lines.push_back ({ Rect::leftTopRightBottom (left, y, right + 1.0f, y + 1.0f), false });
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if(saturation&&panel.equipment){
            for(const auto& [area,title]:equipmentSections){house::paintNineSlice(g,panel.equipment->asset("advanced:section"),area);g.setColour(juce::Colour(0xffe5e6e1));g.setFont(panel.look.caps.fontForCap(16));g.drawText(title,area.reduced(16,8).withHeight(24),juce::Justification::centredLeft);}
        }else{
            for (const auto& l : lines)panel.look.drawEngraving (g, l);
            panel.look.drawTitles (g, titles);
        }
        if (saturation)
        {
            for (const auto& k : knobs)
            {
                g.setFont (panel.look.caps.fontForCap (16));
                g.setColour (juce::Colour (0xffe5e6e1).withAlpha (k->getAlpha()));
                g.drawFittedText (formatParameter (k->index, (float) k->getValue()), k->entry.getBounds(), juce::Justification::centred, 1, .85f);
            }
            return;
        }
        if (! compact) return;
        if(spectralView()){
            const auto area=[&](float x,float y,float w,float h){return detectorArea(getWidth(),x,y,w,h);};
            panel.look.drawGlass(g,area(48,6,984,180));g.setColour(juce::Colour(0xff11191b));g.fillRect(area(56,14,968,164));
            g.setColour(juce::Colour(0xff3d4848));for(const auto x:{276.f,500.f,724.f})g.fillRect(area(x,18,1,140));
            g.fillRect(area(60,158,960,1));g.setColour(juce::Colour(0xffaebfbb));g.setFont(panel.look.caps.fontForCap(12));
            g.drawFittedText("Ordinary bands first > spectral bands last. Spectral bypass retains delay.",area(60,161,960,14).toNearestInt(),juce::Justification::centred,1);
            g.setFont(panel.look.caps.fontForCap(11));
            g.drawText("0 %",area(296,130,80,18),juce::Justification::centredLeft);g.drawText("100 %",area(400,130,80,18),juce::Justification::centredRight);
            g.drawText("-6",area(520,130,80,18),juce::Justification::centredLeft);g.drawText("+6",area(624,130,80,18),juce::Justification::centredRight);
            return;
        }
        if(detectorView()){
            const auto area=[&](float x,float y,float w,float h){return detectorArea(getWidth(),x,y,w,h);};
            panel.look.drawGlass(g,area(524,12,496,142));
            g.setColour(juce::Colour(0xff11191b));g.fillRect(area(532,20,480,90));
            g.setColour(juce::Colour(0xff3d4848));
            for(int n=1;n<3;++n){const auto cell=area(532+160.f*n,20,1,90);g.fillRect(cell);}
            g.setFont(panel.look.caps.fontForCap(14));g.setColour(juce::Colour(0xffb6beb7));
            g.drawText("FREE FREQUENCY",area(284,12,224,20),juce::Justification::centred);
            const char* names[]{"EFFECTIVE THRESHOLD","ATTACK","RELEASE"};
            g.setFont(panel.look.caps.fontForCap(12));
            for(int n=0;n<3;++n)g.drawText(names[n],area(536+160.f*n,29,152,20),juce::Justification::centred);
            g.setColour(detectorAvailable?juce::Colour(0xff91d7ba):juce::Colour(0xff64716b));
            g.fillEllipse(area(61,130,6,6));
            for(std::size_t n=3;n<5;++n){const auto& k=*knobs[n];g.setFont(panel.look.mono.fontForCap(16));g.setColour(juce::Colour(0xffcde4d8).withAlpha(k.getAlpha()));g.drawFittedText(formatParameter(k.index,float(k.getValue())),k.entry.getBounds(),juce::Justification::centred,1,.85f);}
            return;
        }
        const auto left = 72.f, right = (float) getWidth() - 72.f, pitch = (right - left) / 8;
        g.setFont (panel.look.caps.fontForCap (16)); g.setColour (juce::Colour (0xffb6beb7));
        g.drawText (knobs.size() == 12 ? "FILTER" : "OUTPUT / GAIN", Rect (left, 80, right - left, 24), juce::Justification::centredLeft);
        if (knobs.size() == 12) g.drawText ("DYNAMICS", Rect ((left + right) * .5f + 18, 80, (right - left) * .5f - 18, 24), juce::Justification::centredLeft);
        for (const auto& k : knobs)
        {
            if (k->getBounds().getCentreY() < 70) continue;
            const auto field = knobs.size() == 12 && k->index >= dynamicEqParameterStart && k->index < dynamicEqGainParameterStart ? (k->index - dynamicEqParameterStart) % dynamicEqParametersPerSlot % 11 : 99;
            const bool unused = knobs.size() == 12 && field == 6 && k->getAlpha() < .5f;
            if (k->getPresentation() != ProgrammerKnob::Presentation::rotary && ! unused) continue;
            const auto text = unused ? String ("Not used") : formatParameter (k->index, (float) k->getValue());
            g.setFont (panel.look.mono.fontForCap (unused ? 14.f : 18.f));
            g.setColour (juce::Colour (0xffcde4d8).withAlpha (k->getAlpha()));
            g.drawFittedText (text, Rect (pitch - 8, 28).withCentre ({ k->centre().x, 260 }).toNearestInt(), juce::Justification::centred, 1, .85f);
        }
    }

    /** The rows it lays out, at most the PROGRAMMER's two. */
    int rowCount() const { return (int) std::min<std::size_t> (rows.size(), 2); }
    bool compactSaturation() const { return saturation; }

    /** Row by row. */
    std::vector<std::unique_ptr<ProgrammerKnob>> knobs;
    bool detectorView() const {return compact&&!knobs.empty()&&knobs.front()->index>=dynamicEqDetectorParameterStart&&knobs.front()->index<dynamicEqSpectralParameterStart;}
    bool spectralView() const {return compact&&!knobs.empty()&&knobs.front()->index>=dynamicEqSpectralParameterStart&&knobs.front()->index<spaceReverbParameterStart;}
    void refreshDetector()
    {
        if(spectralView()){
            const auto local=knobs.front()->index-dynamicEqSpectralParameterStart,slot=local/25,band=(local%25-1)/3;
            const auto& values=panel.shown.values;const auto detector=dynamicEqDetectorParameterStart+slot*40+band*5;
            const auto original=dynamicEqParameterStart+slot*89+band*11,extension=dynamicEqExtensionParameterStart+slot*16+band*2;
            const bool eligible=values[original+2]<=2.f/5 && values[extension]==0 && values[detector]==0 && values[detector+1]==0 && values[detector+2]==0;
            const bool requested=values[dynamicEqSpectralParameterStart+slot*25]>=.5f;
            knobs[0]->setEnabled(eligible || values[knobs[0]->index]>=.5f);
            for(std::size_t n=1;n<3;++n){knobs[n]->setEnabled(eligible&&requested);knobs[n]->label.setAlpha(eligible&&requested?1.f:.4f);}
            spectralLabels[0]->setText(eligible?"Internal-band detector":"Use bell/shelf + internal band",juce::dontSendNotification);
            const auto effective=panel.processor.effectiveEqMode(slot)==DynamicEqProcessingMode::spectral?"Spectral":"Ordinary";
            const auto internal=panel.processor.fxMonitorHandle()->sampleRate();
            const auto rate=internal/(panel.processor.effectiveRenderQuality()==RenderQuality::higher?2:1);
            spectralLabels[1]->setText("Effective: "+String(effective)+" / "+(rate>0?String(1000.*panel.processor.getLatencySamples()/rate,1)+" ms":"unprepared"),juce::dontSendNotification);
            spectralLabels[2]->setText(panel.processor.eqPreparationPending(slot)?"Pending: restart processing":"Mode changes apply after restart",juce::dontSendNotification);
            return;
        }
        if(!detectorView())return;
        const auto local=knobs.front()->index-dynamicEqDetectorParameterStart,slot=local/40,band=local%40/5,base=dynamicEqDetectorParameterStart+local;
        const auto& values=panel.shown.values;const auto source=values[base+2];const bool validSource=std::isfinite(source)&&source>=0&&source<=1;
        const int mode=validSource?int(std::lround(source*3)):0;const bool free=mode==1||mode==3,external=mode>=2;
        for(std::size_t n=3;n<5;++n){knobs[n]->setAlpha(free?1.f:.4f);knobs[n]->label.setAlpha(free?1.f:.4f);knobs[n]->setDescription(free?String():String("Stored limit; used in Free detection. Editable for later source changes."));}
        const auto old=dynamicEqParameterStart+slot*89+band*11,extension=dynamicEqExtensionParameterStart+slot*16+band*2;
        const bool active=values[old]>=.5f&&values[old+1]<.5f&&values[572+slot*23]<.5f;
        const bool dynamic=values[old+2]<=2.f/5&&values[extension]==0;
        const auto& meter=panel.detectorMeters[slot];const bool live=meter.valid&&active&&dynamic&&validSource;const auto& r=meter.values[band];
        detectorLabels[0]->setText(live?String(r.thresholdDb,1)+" dB":"--",juce::dontSendNotification);
        detectorLabels[1]->setText(live?String(r.attackMs,1)+" ms":"--",juce::dontSendNotification);
        detectorLabels[2]->setText(live?String(r.releaseMs,1)+" ms":"--",juce::dontSendNotification);
        String status=external?(meter.externalAvailable?"External input available":"External input unavailable"):"Internal trigger";
        if(!validSource)status="Waiting for valid controls";else if(!active)status="Band inactive; "+status;else if(!dynamic)status="Static band; "+status;
        detectorLabels[3]->setText(status,juce::dontSendNotification);
        const bool available=validSource&&(!external||meter.externalAvailable);if(detectorAvailable!=available){detectorAvailable=available;repaint();}
    }

private:
    void layoutSaturation()
    {
        lines.clear();titles={};equipmentSections.clear();
        const float left=16,right=(float)getWidth()-16;
        std::size_t n=0;
        for(std::size_t r=0;r<rows.size();++r){
            std::size_t count=0;for(const auto& section:rows[r])count+=section.knobs.size();if(count==0)continue;
            const auto pitch=(right-left)/(float)count,y=164.f+190.f*(float)r;
            std::size_t column=0;
            for(const auto& section:rows[r]){
                const auto x0=left+(float)column*pitch,x1=x0+(float)section.knobs.size()*pitch;
                equipmentSections.push_back({Rect(x0+4,y-102,x1-x0-8,174).toNearestInt(),section.title});
                for(const auto index:section.knobs){
                    auto& k=*knobs[n++];const auto x=left+((float)column+++.5f)*pitch;using Face=ProgrammerKnob::Presentation;
                    const auto kind=parameterRegistry()[index].kind;
                    k.setPresentation(kind==ParameterKind::continuous?Face::rotary:kind==ParameterKind::toggle?Face::toggle:Face::choice);
                    k.setEquipmentArtwork(panel.equipment);if(String(parameterRegistry()[index].id).contains(".saturation.")&&String(parameterRegistry()[index].id).endsWith(".bypassed"))k.label.setText("LANE BYPASS",juce::dontSendNotification);k.place({x,y});
                    k.setBounds(Rect(k.getPresentation()==Face::rotary?80.f:132.f,k.getPresentation()==Face::rotary?80.f:54.f).withCentre({x,y}).toNearestInt());
                    k.label.setBounds(Rect(pitch-20,24).withCentre({x,y-52}).toNearestInt());
                    k.entry.setBounds(Rect(pitch-20,24).withCentre({x,y+52}).toNearestInt());
                }
            }
        }
        repaint();
    }
    void layoutEq()
    {
        lines.clear(); titles = {};
        const float left = 72, right = (float) getWidth() - 72, pitch = (right - left) / 8;
        using Face = ProgrammerKnob::Presentation;
        lines.push_back ({ { left, 72, right - left, 1 }, false });
        if(spectralView()){
            lines.clear();const auto area=[&](float x,float y,float w,float h){return detectorArea(getWidth(),x,y,w,h);};
            const auto place=[&](std::size_t n,Rect bounds,Face face,Rect label){auto& k=*knobs[n];k.setPresentation(face);k.place(bounds.getCentre());k.setBounds(bounds.toNearestInt());k.label.setBounds(label.toNearestInt());k.entry.setBounds(bounds.toNearestInt());k.label.setTopLeftPosition(k.label.getX(),juce::roundToInt(float(k.label.getY())+label.getCentreY()-k.labelInk().getCentreY()));};
            place(0,area(72,68,184,30),Face::toggle,area(78,68,122,30));knobs[0]->getProperties().set("programmer.digitalToggle",true);
            for(std::size_t n=1;n<3;++n){const float x=n==1?296.f:520.f;place(n,area(x,58,184,70),Face::horizontal,area(x,30,184,20));}
            place(3,area(744,55,264,28),Face::choice,area(744,28,264,20));knobs[3]->getProperties().set("programmer.choiceCap",14.f);
            for(std::size_t n=0;n<4;++n)knobs[n]->setExplicitFocusOrder(5+int(n));
            spectralLabels[0]->setBounds(area(72,110,184,30).toNearestInt());spectralLabels[1]->setBounds(area(744,90,264,22).toNearestInt());spectralLabels[2]->setBounds(area(744,123,264,20).toNearestInt());
            refreshDetector();repaint();return;
        }
        if(detectorView()){
            lines.clear();
            const auto area=[&](float x,float y,float w,float h){return detectorArea(getWidth(),x,y,w,h);};
            const auto place=[&](std::size_t n,Rect bounds,Face face,Rect label){auto& k=*knobs[n];k.setPresentation(face);k.place(bounds.getCentre());k.setBounds(bounds.toNearestInt());k.label.setBounds(label.toNearestInt());k.entry.setBounds(bounds.toNearestInt());k.label.setTopLeftPosition(k.label.getX(),juce::roundToInt(float(k.label.getY())+label.getCentreY()-k.labelInk().getCentreY()));};
            const char* names[]{"THRESHOLD AUTO","ADAPTIVE TIMING","SOURCE","LOW","HIGH"};
            for(std::size_t n=0;n<5;++n)knobs[n]->label.setText(names[n],juce::dontSendNotification);
            for(std::size_t n=0;n<2;++n){const float x=536+236.f*float(n);place(n,area(x,118,236,26),Face::toggle,area(x+4,118,204,26));knobs[n]->getProperties().set("programmer.softKey",true);knobs[n]->setExplicitFocusOrder(9+int(n));}
            place(2,area(60,77,208,32),Face::choice,area(60,54,208,19));knobs[2]->getProperties().set("programmer.choiceCap",14.f);knobs[2]->setExplicitFocusOrder(5);
            for(std::size_t n=3;n<5;++n){auto& k=*knobs[n];const float x=340+112.f*float(n-3);k.setPresentation(Face::rotary);k.place(area(x,85,0,0).getPosition());k.setBounds(area(x-32,53,64,64).toNearestInt());k.label.setBounds(area(x-54,36,108,20).toNearestInt());k.entry.setBounds(area(x-54,120,108,24).toNearestInt());k.setExplicitFocusOrder(3+int(n));}
            for(int n=0;n<3;++n)detectorLabels[std::size_t(n)]->setBounds(area(536+160.f*n,58,152,36).toNearestInt());
            detectorLabels[3]->setBounds(area(74,123,194,23).toNearestInt());refreshDetector();repaint();return;
        }
        if (knobs.size() == 12) lines.push_back ({ { (left + right) * .5f, 82, 1, 200 }, true });
        const auto header = [&] (ProgrammerKnob& k, Rect bounds, Face face, Rect label)
        {
            k.setPresentation (face); k.place (bounds.getCentre()); k.setBounds (bounds.toNearestInt());
            k.label.setBounds (label.toNearestInt()); k.entry.setBounds (bounds.toNearestInt());
            k.label.setTopLeftPosition (k.label.getX(), juce::roundToInt ((float) k.label.getY() + bounds.getCentreY() - k.labelInk().getCentreY()));
        };
        for (std::size_t n = 0; n < knobs.size(); ++n)
        {
            auto& k = *knobs[n];
            if (knobs.size() == 12)
            {
                if (k.index >= dynamicEqPlacementParameterStart)
                { header (k, { left + 922, 15, right - left - 922, 38 }, Face::segments, { left + 810, 22, 100, 24 }); k.setExplicitFocusOrder (8); continue; }
                const auto field = (k.index - dynamicEqParameterStart) % dynamicEqParametersPerSlot % 11;
                if (field < 2)
                { const auto x = left + 324 + (float) field * 106; header (k, { x, 15, 98, 38 }, Face::toggle, { x + 26, 22, 70, 24 }); k.setExplicitFocusOrder (5 + (int) field); continue; }
                if (field == 2)
                { header (k, { left + 616, 15, 170, 38 }, Face::choice, { left + 552, 22, 58, 24 }); k.setExplicitFocusOrder (7); continue; }
            }
            const auto field = knobs.size() == 12 ? (k.index - dynamicEqParameterStart) % dynamicEqParametersPerSlot % 11 : n + 3;
            const auto x = left + ((float) field - 3 + .5f) * pitch;
            k.setPresentation (parameterRegistry()[k.index].kind == ParameterKind::continuous ? Face::rotary : Face::choice);
            k.setExplicitFocusOrder ((int) field + 6);
            k.place ({ x, 189 });
            if (k.getPresentation() != Face::rotary) k.setBounds (Rect (140, 38).withCentre ({ x, 189 }).toNearestInt());
            k.label.setBounds (Rect (pitch - 4, 24).withCentre ({ x, 122 }).toNearestInt());
            k.entry.setBounds (Rect (pitch - 8, 28).withCentre ({ x, 260 }).toNearestInt());
        }
        repaint();
    }
    AllControlsPanel& panel;
    std::vector<std::vector<Section>> rows;
    std::vector<Engraving> lines;
    TitleRow titles;
    std::vector<std::pair<juce::Rectangle<int>,String>> equipmentSections;
    bool compact = false, saturation = false;
    std::array<std::unique_ptr<juce::Label>,4> detectorLabels;
    std::array<std::unique_ptr<juce::Label>,3> spectralLabels;
    bool detectorAvailable = false;
};

class AllControlsPanel::PatchBay final : public KeyFocus<juce::TextButton>
{
public:
    explicit PatchBay(const house::Model* art) : KeyFocus<juce::TextButton>("Patch Bay")
    {
        setArt(art);
        setComponentID("rack.controls.patchbay");setTitle("Patch Bay service cover: design and mappings");
        setWantsKeyboardFocus(true);setExplicitFocusOrder(6);setToggleable(true);setClickingTogglesState(false);
        showOpen(false);
    }
    void setArt(const house::Model* art){
        cover={};latch={};
        if(art){const auto c=art->assets.find("patch-bay:cover"),l=art->assets.find("patch-bay:latch");if(c!=art->assets.end()&&l!=art->assets.end()){cover=c->second.image;latch=l->second.image;}}
        getProperties().set("sourceLoaded",cover.isValid()&&latch.isValid());repaint();
    }
    void showOpen(bool open)
    {
        if(getToggleState()==open&&getDescription().isNotEmpty())return;
        setToggleState(open,juce::dontSendNotification);
        setDescription(open?"Patch Bay open. Existing design and mapping controls are exposed. Close with Space, Enter or Escape.":"Patch Bay closed. Open the service cover with Space or Enter for existing design and mapping controls.");
        repaint();
    }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if(key.isKeyCode(juce::KeyPress::spaceKey)||key.isKeyCode(juce::KeyPress::returnKey)||(getToggleState()&&key.isKeyCode(juce::KeyPress::escapeKey))){triggerClick();return true;}
        return false;
    }
    void paintButton(juce::Graphics& g,bool hover,bool down) override
    {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        const auto visual=getLocalBounds().toFloat().reduced(14,3);
        if(cover.isValid())g.drawImage(cover,visual);
        else{g.setColour(juce::Colour(0xff252c2d));g.fillRect(visual);g.setColour(juce::Colours::white);g.drawText("PATCH BAY",visual,juce::Justification::centred);}
        if(getToggleState())
        {
            const auto sx=visual.getWidth()/248.f,sy=visual.getHeight()/54.f;
            const auto pivot=visual.getPosition()+Pt(214.5f*sx,26.f*sy);
            g.drawImageTransformed(latch,juce::AffineTransform::translation(-.5f*latch.getWidth(),-.5f*latch.getHeight()).rotated(juce::MathConstants<float>::halfPi).scaled(sx,sy).translated(pivot.x,pivot.y));
        }
        if(hover||down){g.setColour(juce::Colours::white.withAlpha(down?.06f:.025f));g.fillRect(getLocalBounds().reduced(3));}
        if(ringed()){g.setColour(accent);g.drawRect(getLocalBounds().reduced(1),2);}
    }
private:
    juce::Image cover,latch;
};

class AllControlsPanel::EquipmentKey final : public KeyFocus<juce::TextButton>
{
public:
    EquipmentKey(const house::Model* m,const ProgrammerLook& l):art(m),look(l){}
    void setArt(const house::Model* m){art=m;repaint();}
    void paintButton(juce::Graphics& g,bool hover,bool down) override
    {
        if(!art)return juce::TextButton::paintButton(g,hover,down);
        const auto visual=getLocalBounds().reduced(6,4);
        house::paintNineSlice(g,art->asset("advanced:key"),visual);
        if(hover||down){g.setColour(juce::Colours::white.withAlpha(down?.06f:.025f));g.fillRect(getLocalBounds().reduced(4));}
        g.setColour(juce::Colour(0xffe5e6e1));g.setFont(look.caps.fontForCap(14));
        g.drawFittedText(getButtonText(),visual.reduced(8,4),juce::Justification::centred,1,.85f);
        if(getToggleState())g.drawLine((float)visual.getX()+14,(float)visual.getBottom()-6,(float)visual.getRight()-14,(float)visual.getBottom()-6,2.f);
        if(ringed()){g.setColour(accent);g.drawRect(getLocalBounds().reduced(1),2);}
    }
private:
    const house::Model* art=nullptr;const ProgrammerLook& look;
};

AllControlsPanel::AllControlsPanel (InstrumentProcessor& p, const Model& m, ProgrammerHost h)
    : processor (p), model (m), look (m), host (std::move (h))
{
    setComponentID ("rack.controls");
    setTitle ("All controls");
    setOpaque (true);
    setFocusContainerType (FocusContainerType::focusContainer);
    setLookAndFeel (&look);
    line.setComponentID ("rack.controls.line");
    ProgrammerLook::setRole (line, ProgrammerLook::Text::lines);
    line.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (line);
    patchBay=std::make_unique<PatchBay>(equipment);addChildComponent(*patchBay);
    saturationGraph=std::make_unique<SaturationGraph>(processor,host.house,equipmentOwner);addChildComponent(*saturationGraph);
    patchBay->onClick=[this]{if(onPatchBay)onPatchBay(!patchBay->getToggleState());};
    previous = std::make_unique<KeyFocus<juce::TextButton>> ("<"); next = std::make_unique<KeyFocus<juce::TextButton>> (">");
    previous->setComponentID ("rack.controls.previous"); next->setComponentID ("rack.controls.next");
    ProgrammerLook::setCompactButton (*previous); ProgrammerLook::setCompactButton (*next);
    for (auto* c : std::initializer_list<juce::Component*> { previous.get(), next.get(), &unitChoice, &bandChoice }) c->setWantsKeyboardFocus (true);
    previous->setTitle ("Previous EQ page"); next->setTitle ("Next EQ page");
    previous->onClick = [this] { selectEqPage ((routesPage() + 17) % 18); };
    next->onClick = [this] { selectEqPage ((routesPage() + 1) % 18); };
    unitChoice.setComponentID ("rack.controls.unit"); unitChoice.setTitle ("Rack unit"); unitChoice.setExplicitFocusOrder (1);
    previous->setExplicitFocusOrder (2); bandChoice.setExplicitFocusOrder (3); next->setExplicitFocusOrder (4);
    bandChoice.setComponentID ("rack.controls.band"); bandChoice.setTitle ("EQ band, output or gain page");
    for (int n = 0; n < 8; ++n) bandChoice.addItem ("BAND " + String (n + 1), n + 1);
    bandChoice.addItem ("OUTPUT", 9); bandChoice.addItem ("GAIN", 10);
    for(int n=0;n<8;++n)bandChoice.addItem("DETECTOR "+String(n+1),11+n);
    for(int n=0;n<8;++n)bandChoice.addItem("SPECTRAL "+String(n+1),19+n);
    unitChoice.onChange = [this] { const auto chosen = unitChoice.getSelectedId() - 1; if (chosen != key) showKey (chosen); };
    bandChoice.onChange = [this] { selectEqPage (bandChoice.getSelectedId() - 1); };
    for (auto* c : std::initializer_list<juce::Component*> { previous.get(), next.get(), &unitChoice, &bandChoice }) addChildComponent (c);
    setSize (1485, panelHeight);
}

AllControlsPanel::~AllControlsPanel()
{
    retireGraph();
    for(auto* knob:byIndex)if(knob)knob->closeEntry();
    keyList.clear();
    views.clear();
    unitChoice.clear (juce::dontSendNotification);
    setLookAndFeel (nullptr);
}

void AllControlsPanel::show (std::vector<Unit> units)
{
    MakerSkinPtr nextOwner;
    SoundDocument current;std::uint64_t edit;
    if(processor.trySoundSnapshot(current,edit)&&current.personalities)for(const auto& rack:current.personalities->instances){
        const auto owned=panelUnits(rack);if(!units.empty()&&std::all_of(units.begin(),units.end(),[&](const auto& u){return std::any_of(owned.begin(),owned.end(),[&](const auto& binding){return binding.module==u.module&&binding.bank==u.bank;});})){
            processor.requestMakerSkin(rack);nextOwner=processor.preparedMakerSkin(rack);break;
        }
    }
    if(nextOwner)for(const auto* key:{"advanced:plate","advanced:section","advanced:key"})if(!nextOwner->skin.assets.contains(key)){nextOwner.reset();break;}
    const auto previousOwner=equipmentOwner;
    if(nextOwner!=equipmentOwner){
        equipmentOwner=std::move(nextOwner);equipment=equipmentOwner?&equipmentOwner->skin:nullptr;
        patchBay->setArt(equipment);saturationGraph->setEquipmentArtwork(equipmentOwner);
        for(const auto& view:views)if(view->compactSaturation())for(const auto& knob:view->knobs)knob->setEquipmentArtwork(equipment);
        for(const auto& item:keyList)if(auto* button=dynamic_cast<EquipmentKey*>(item.button.get()))button->setArt(equipment);
        resized();repaint();
    }
    if(units==shownUnits)return;
    shownUnits = std::move (units);
    keyList.clear();
    views.clear();
    unitChoice.clear (juce::dontSendNotification);
    byIndex.fill (nullptr);
    placeOf.clear();
    lastTouched = nullptr;
    line.setText ({}, juce::dontSendNotification);
    key = 0;
    const auto rowsOf = [] (const SubpageSpec& s)
    {
        std::vector<std::vector<const SectionSpec*>> rows;
        for (const auto& row : s.rows)
        {
            auto& r = rows.emplace_back();
            for (const auto& section : row)
                r.push_back (&section);
        }
        return rows;
    };
    for (const auto& u : shownUnits)
    {
        const auto reach = unitReach (u.module, u.bank, u.revision);
        if (isSourceModule (u.module))
        {
            const auto reached = [&reach] (const SubpageSpec& s)
            {
                for (const auto& row : s.rows)
                    for (const auto& section : row)
                        for (const auto& k : section.knobs)
                            if (std::binary_search (reach.begin(), reach.end(), (std::size_t) k.index))
                                return true;
                return false;
            };
            for (const auto& sub : model.page ("synth" + String (u.bank + 1)).subpages)
            {
                if (u.module == NativeModule::drum) {
                    if (sub.id == "common" && reached (sub)) addKey (sub.label, {&addView (rowsOf (sub), reach)}, u);
                    if (sub.id == "drum") {
                        std::vector<View*> pages;
                        for (const auto& page : sub.routes) if (page.id.startsWith ("drum.pad.") && reached (page))
                            pages.push_back (&addView (rowsOf (page), reach));
                        if (!pages.empty()) addKey (sub.label, std::move (pages), u);
                    }
                    continue;
                }
                if(sub.id=="drum")continue;
                if (sub.engine >= 0 && sub.engine != (u.module == NativeModule::wavetable ? 1 : 0))
                    continue;
                if (! reached (sub) && std::none_of (sub.routes.begin(), sub.routes.end(), reached))
                    continue;
                std::vector<View*> pages;
                if (sub.routes.empty())
                    pages.push_back (&addView (rowsOf (sub), reach));
                for (const auto& r : sub.routes)
                    pages.push_back (&addView (rowsOf (r), reach));
                addKey (sub.label, std::move (pages), u);
            }
            continue;
        }
        const auto kind = effectKindForModule(u.module);
        for (const auto& sub : model.page ("effects").subpages)
        {
            if (sub.effect != u.bank)
                continue;
            // SLOT is on FX N's first page only.
            std::vector<const SectionSpec*> row;
            const auto add = [&row] (const SubpageSpec& page, const String& title)
            {
                for (const auto& r : page.rows)
                    for (const auto& section : r)
                        if (section.title == title)
                            row.push_back (&section);
            };
            for (const auto& page : sub.routes)
                add (page, "SLOT");
            if (kind == EffectKind::saturation)
            {
                for (const auto& page : sub.routes)
                    if (page.id.startsWith ("saturation."))
                    {
                        auto rows = rowsOf (page);
                        if (page.id == "saturation.global")
                            rows.front().insert (rows.front().begin(), row.begin(), row.end());
                        addKey (page.id.fromFirstOccurrenceOf (".", false, false).toUpperCase(), { &addView (rows, reach, false, true) }, u);
                    }
                continue;
            }
            else if (kind == EffectKind::spaceReverb || kind == EffectKind::eraReverb)
            {
                const String prefix = kind == EffectKind::spaceReverb ? "space_reverb." : "era_reverb.";
                std::vector<View*> pages;
                for (const auto& page : sub.routes)
                    if (page.id.startsWith (prefix))
                    {
                        auto rows = rowsOf (page);
                        if (page.id == prefix + "main") rows.front().insert (rows.front().begin(), row.begin(), row.end());
                        pages.push_back (&addView (rows, reach));
                    }
                addKey (sub.label, std::move (pages), u);
                if (kind == EffectKind::spaceReverb) keyList.back().spaceSlot = u.bank;
                continue;
            }
            if (kind == EffectKind::dynamicEq)
            {
                std::vector<View*> pages;
                for (const auto& page : sub.routes)
                    if (page.id.startsWith ("dynamic_eq.band.") || page.id.startsWith("dynamic_eq.detector.") || page.id.startsWith("dynamic_eq.spectral.") || page.id == "dynamic_eq.gain")
                        pages.push_back (&addView (rowsOf (page), reach, true));
                    else if (page.id == "dynamic_eq.output")
                    {
                        auto output = rowsOf (page);
                        output.front().insert (output.front().begin(), row.begin(), row.end());
                        pages.push_back (&addView (output, reach, true));
                    }
                addKey (sub.label, std::move (pages), u);
                keyList.back().eqSlot = u.bank;
                continue;
            }
            add (sub.routes[(std::size_t) pageOfKind (sub, kind)], effectDisplayName (kind).toUpperCase());
            std::vector<std::vector<const SectionSpec*>> rows { row };
            if (kind == EffectKind::delay)
                for (const auto& page : sub.routes)
                    if (page.id == "delay.tape" || page.id == "delay.modes")
                        rows.push_back (rowsOf (page).front());
            addKey (sub.label, { &addView (rows, reach) }, u);
        }
    }
    resized();
    showShown();
}

AllControlsPanel::View& AllControlsPanel::addView (const std::vector<std::vector<const SectionSpec*>>& rows, const std::vector<std::size_t>& reach, bool compactEq, bool compactSaturation)
{
    std::vector<std::vector<View::Section>> kept;
    for (const auto& row : rows)
    {
        auto& r = kept.emplace_back();
        for (const auto* section : row)
        {
            View::Section s { section->title, {} };
            for (const auto& k : section->knobs)
                if (std::binary_search (reach.begin(), reach.end(), (std::size_t) k.index))
                    s.knobs.push_back ((std::size_t) k.index);
            if (! s.knobs.empty())
                r.push_back (std::move (s));
        }
    }
    auto& v = *views.emplace_back (std::make_unique<View> (*this, std::move (kept), compactEq, compactSaturation));
    v.setExplicitFocusOrder (100);
    addChildComponent (v);
    for (auto& k : v.knobs)
    {
        byIndex[k->index] = k.get();
        if(const auto extra=k->pairedParameter())byIndex[*extra]=k.get();
        wire (*k);
    }
    return v;
}

void AllControlsPanel::addKey (const String& label, std::vector<View*> pages, const Unit& unit)
{
    const auto n = (int) keyList.size();
    for (std::size_t p = 0; p < pages.size(); ++p)
        for (auto& k : pages[p]->knobs){
            placeOf[k->index] = { n, (int) p };
            if(const auto extra=k->pairedParameter())placeOf[*extra]={n,(int)p};
        }
    std::unique_ptr<juce::Button> button;
    if(unit.module==NativeModule::saturation)button=std::make_unique<EquipmentKey>(equipment,look);
    else button=std::make_unique<KeyFocus<juce::ToggleButton>>();
    auto& b = *keyList.emplace_back (Key { std::move(button), std::move (pages) }).button;
    b.setButtonText (label);
    if(unit.module==NativeModule::saturation)keyList.back().saturationSlot=unit.bank;
    const auto bank=String(isSourceModule(unit.module)?"SYNTH ":"FX ")+String(unit.bank+1);
    unitChoice.addItem (bank+(label.startsWith("FX ")?String():" / "+label), n + 1);
    b.setTitle (label);
    b.setComponentID ("rack.controls.key." + String (n));
    b.setClickingTogglesState (false);
    b.setToggleable (true);
    b.setExplicitFocusOrder (1 + n);
    b.onClick = [this, n] { showKey (n); };
    b.setBounds (keyX + keyPitch * n, keyY, keyWidth, keyHeight);
    addAndMakeVisible (b);
}

void AllControlsPanel::wire (ProgrammerKnob& k)
{
    const auto i = k.index;
    k.setLocked (shown.controlLocks[i]);
    k.onTouched = [this] (ProgrammerKnob& touched)
    {
        lastTouched = &touched;
        line.setText (touched.valueLine(), juce::dontSendNotification);
        if (auto* view = touched.getParentComponent()) view->repaint();
    };
    if (i >= 252 && i < 572 && (i - 252) % 5 != 4)
        k.onEdit = [this, i] (float physical) { editRoute (processor, host, i, processor.control (i).convertTo0to1 (physical), [this] { rereadRoutes(); }); };
    if (parameterRegistry()[i].kind != ParameterKind::continuous)
        return;
    k.addToMenu = [this, stateKey = parameterRegistry()[i].stateKey] (ProgrammerKnob::Menu& menu)
    {
        for (std::size_t slot = 0; slot < 8; ++slot)
            menu.push_back ({ "Assign to MACRO " + String ((int) slot + 1), [this, stateKey, slot]
                              {
                                  const auto scope = host.house.frame ? host.house.frame() : nullptr;
                                  if (! host.house.change || (host.house.closing && host.house.closing()))
                                      return;
                                  host.house.change ([stateKey, slot] (SoundDocument& d)
                                                     {
                                                         auto& b = d.macros[slot];
                                                         b = { stateKey, b.targetKey != 0 ? b.amount : 0.5f };
                                                     },
                                                     "Macro assignment");
                              } });
    };
}

std::vector<String> AllControlsPanel::keys() const
{
    std::vector<String> out;
    for (const auto& k : keyList)
        out.push_back (k.button->getButtonText());
    return out;
}

void AllControlsPanel::showKey (int n)
{
    if (n < 0 || n >= (int) keyList.size())
        return;
    if (n == key)
    {
        auto& k = keyList[(std::size_t) n];
        k.page = (k.page + 1) % (int) k.pages.size();
    }
    key = n;
    showShown();
    const auto& selected = keyList[(std::size_t) key];
    if (selected.spaceSlot >= 0 && selected.page >= 2 && onSpaceBandSelected && ! (host.house.closing && host.house.closing()))
    {
        const auto callback = onSpaceBandSelected;
        callback ((std::size_t) selected.spaceSlot, selected.page >= 8, (std::size_t) ((selected.page - 2) % 6));
    }
}

bool AllControlsPanel::showParameter(std::size_t index)
{
    const auto found=placeOf.find(index);if(found==placeOf.end())return false;
    key=found->second.first;keyList[std::size_t(key)].page=found->second.second;showShown();return true;
}

int AllControlsPanel::shownHeight() const
{
    if (saturationGlobal()) return 634;
    if (compactEq()) return spectralPage()?274:detectorPage() ? 237 : 292;
    const auto* v = shownView();
    if (v != nullptr && v->compactSaturation()) return 254 + 190 * (v->rowCount() - 1);
    const auto rows = v != nullptr ? std::max (1, v->rowCount()) : 1;
    return (int) std::lround (num (model.programmer["body"]["rows"][rows - 1]["divider"][1]) - rise) + 20;
}

const AllControlsPanel::View* AllControlsPanel::shownView() const
{
    if (keyList.empty())
        return nullptr;
    const auto& k = keyList[(std::size_t) key];
    return k.pages[(std::size_t) k.page];
}

int AllControlsPanel::routesPage() const { return keyList.empty() ? 0 : keyList[(std::size_t) key].page; }
bool AllControlsPanel::detectorPage() const {const auto* view=shownView();return compactEq()&&view&&view->detectorView();}
bool AllControlsPanel::spectralPage() const {const auto* view=shownView();return compactEq()&&view&&view->spectralView();}
void AllControlsPanel::setDetectorReadbacks(std::uint8_t slot,const std::array<DynamicEqDetectorReadback,8>& values,bool valid,bool external)
{
    if(slot>=8)return;detectorMeters[slot]={values,valid,external};
    for(auto& v:views)if(v->isVisible())v->refreshDetector();
}

ProgrammerKnob* AllControlsPanel::knob (std::size_t index) const {
    for(const auto& view:views)if(view->isVisible())for(const auto& knob:view->knobs)if(knob->index==index)return knob.get();
    return index < byIndex.size() ? byIndex[index] : nullptr;
}

std::vector<ProgrammerKnob*> AllControlsPanel::shownKnobs() const
{
    std::vector<ProgrammerKnob*> out;
    for (const auto& v : views)
        if (v->isVisible())
            for (const auto& k : v->knobs)
                out.push_back (k.get());
    return out;
}

std::vector<std::size_t> AllControlsPanel::parameters() const
{
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < byIndex.size(); ++i)
        if (byIndex[i] != nullptr)
            out.push_back (i);
    return out;
}

void AllControlsPanel::refresh (const SoundDocument& d)
{
    // Called every 20 ms; the knobs follow their parameters, and the marks move only with the values or the locks.
    const bool moved = d.values != shown.values || d.controlLocks != shown.controlLocks;
    if (&d != &shown)
        shown = d;
    for (const auto& view:views)
        for (const auto& owning:view->knobs)if(auto* k=owning.get())
        {
            const auto locked=d.controlLocks[k->index]||(k->pairedParameter()&&d.controlLocks[*k->pairedParameter()]);
            if(k->isLocked()!=locked)k->setLocked(locked);
            showDynamicEqRelevance(*k,d.values);
            showSaturationRelevance(*k,d);
            if(!showSpaceReverbRelevance(*k,d.values))return;
        }
    if(saturationGlobal())saturationGraph->refresh(d,std::uint8_t(keyList[std::size_t(key)].saturationSlot));
    setDescription(String());
    line.setText (lastTouched?lastTouched->valueLine():String(), juce::dontSendNotification);
    if (moved)
        repaint();
    for(auto& v:views)if(v->isVisible())v->refreshDetector();
}

void AllControlsPanel::showProposal (std::shared_ptr<const SoundDocument> candidate)
{
    proposal = std::move (candidate);
    saturationGraph->showProposal(proposal);
    repaint();
}

std::vector<AllControlsPanel::Mark> AllControlsPanel::marks() const
{
    std::vector<Mark> out;
    if (proposal == nullptr)
        return out;
    for (std::size_t i = 0; i < byIndex.size(); ++i)
        if (byIndex[i] != nullptr && ! shown.controlLocks[i] && std::abs (proposal->values[i] - shown.values[i]) > 1.0e-6f)
            out.push_back ({ i, processor.control (i).convertFrom0to1 (proposal->values[i]) });
    return out;
}

std::vector<int> AllControlsPanel::outlinedKeys() const
{
    std::vector<int> out;
    for (const auto& m : marks())
        if (const auto it = placeOf.find (m.index); it != placeOf.end())
        {
            const auto [k, page] = it->second;
            const auto* control=knob(m.index);const bool visible=control&&control->getParentComponent()->isVisible();
            if (!visible && (k != key || page != keyList[(std::size_t) k].page) && std::find (out.begin(), out.end(), k) == out.end())
                out.push_back (k);
        }
    std::sort (out.begin(), out.end());
    return out;
}

void AllControlsPanel::paint (juce::Graphics& g)
{
    if(equipment&&shownView()!=nullptr&&shownView()->compactSaturation())house::paintNineSlice(g,equipment->asset("advanced:plate"),getLocalBounds());
    else look.drawFace (g, face.isEmpty() ? getLocalBounds().toFloat() : face);
    if(saturationGlobal()&&equipment)house::paintNineSlice(g,equipment->asset("advanced:section"),{20,436,getWidth()-40,186});
    if (compactEq() && keyList.size() == 1 && !detectorPage()&&!spectralPage())
    {
        g.setColour (juce::Colour (0xffb7bdb9)); g.setFont (look.caps.fontForCap (19));
        g.drawText (keyList[(std::size_t) key].button->getButtonText(), Rect (72, 15, 74, 38), juce::Justification::centredLeft);
    }
}
void AllControlsPanel::setFace (Rect area)
{
    if (area == face)
        return;
    face = area;
    repaint();
}

// Each mark dashed on its own, since a dashed stroke joins its sub-paths.
void AllControlsPanel::paintOverChildren (juce::Graphics& g)
{
    std::bitset<parameterCount> painted;
    for (const auto& m : marks())
        if (auto* k = knob(m.index); k != nullptr && k->getParentComponent()->isVisible())
        {
            if(painted[k->index])continue;painted.set(k->index);
            const auto r = getLocalArea (k, k->getLocalBounds()).toFloat();
            if (k->getPresentation() != ProgrammerKnob::Presentation::rotary)
            { juce::Path box; box.addRectangle (r.reduced (1)); ProgrammerLook::drawDashes (g, box, markColour, 6, 4); continue; }
            const auto a = juce::degreesToRadians (k->angleAt (m.proposed));
            juce::Path p;
            p.startNewSubPath (r.getCentre());
            p.lineTo (r.getCentre() + Pt (std::sin (a), -std::cos (a)) * (r.getWidth() * 0.5f));
            ProgrammerLook::drawDashes (g, p, markColour, 6.0f, 4.0f);
        }
    for (const auto n : outlinedKeys())
    {
        juce::Path p;
        const auto area = keyList[(std::size_t)n].button->isVisible()?keyList[(std::size_t)n].button->getBounds():(compactEq()&&n==key?bandChoice.getBounds():unitChoice.getBounds());
        p.addRectangle (area.toFloat().expanded (3.0f));
        ProgrammerLook::drawDashes (g, p, markColour, 6.0f, 4.0f);
    }
}

void AllControlsPanel::setPatchBayOpen(bool open){patchBay->showOpen(open);}
bool AllControlsPanel::focusPatchBay(){if(!patchBay->isShowing())return false;patchBay->grabKeyboardFocus();return true;}
bool AllControlsPanel::patchBayVisible() const{return patchBay->isVisible();}

void AllControlsPanel::resized()
{
    if(saturationGraph)saturationGraph->setBounds(28,440,std::max(1,getWidth()-56),182);
    const bool mixed=serviceCover()&&shownUnits.size()>1;
    unitChoice.setBounds (mixed&&!compactEq()?32:72, 15, mixed&&!compactEq()?166:74, 38); previous->setBounds (154, 15, 28, 38);
    const bool native=equipment&&shownView()!=nullptr&&shownView()->compactSaturation();
    int shownButton=0;
    for(std::size_t n=0;n<keyList.size();++n){auto& button=*keyList[n].button;const bool keyArt=equipment&&keyList[n].saturationSlot>=0;
        const auto x=native?(mixed?216:32)+114*shownButton:(mixed?260+keyPitch*shownButton:keyX+keyPitch*(int)n);
        button.setBounds(x,keyArt?11:keyY,keyArt?112:keyWidth,keyArt?46:keyHeight);
        if(button.isVisible())++shownButton;
    }
    bandChoice.setBounds (188, 15, 148, 38); next->setBounds (342, 15, 28, 38);
    if(spectralPage()){
        const auto area=[&](float x,float y,float w,float h){return detectorArea(getWidth(),x,y,w,h).toNearestInt();};
        previous->setBounds(area(72,30,26,28));bandChoice.setBounds(area(104,30,120,28));next->setBounds(area(230,30,26,28));
    }else if(detectorPage()){
        const auto area=[&](float x,float y,float w,float h){return detectorArea(getWidth(),x,y,w,h).toNearestInt();};
        const bool multiple=keyList.size()>1;
        unitChoice.setBounds(area(60,18,52,28));previous->setBounds(area(multiple?118.f:60.f,18,26,28));
        bandChoice.setBounds(area(multiple?150.f:92.f,18,multiple?86.f:144.f,28));next->setBounds(area(242,18,26,28));
    }

    for (auto& v : views)
    {
        v->setBounds (getLocalBounds());
        v->layout();
    }
    if(spectralPage())for(auto* component:std::initializer_list<juce::Component*>{previous.get(),next.get(),&bandChoice,&unitChoice})component->toFront(false);
    const auto x = native?(mixed?216:32)+114*shownButton:mixed?260+keyPitch*shownButton:keyX + keyPitch * (int) keyList.size();
    patchBay->setBounds(getWidth()-(native?8:72)-248,native?8:28,248,54);
    const bool sat=serviceCover();
    line.setBounds (x, native?28:sat?38:lineY, std::max (0, (sat?patchBay->getX()-20:getWidth()-(int)bodyInset)-x), lineHeight);
}

void AllControlsPanel::showShown()
{
    const juce::Component::SafePointer<AllControlsPanel> alive(this);
    const View* visible = shownView();
    const bool mixed=serviceCover()&&shownUnits.size()>1, saturation=visible!=nullptr&&visible->compactSaturation();
    for (auto& v : views)
    {
        if(v.get()!=visible)for(auto& knob:v->knobs){knob->closeEntry();if(!alive)return;}
        v->setVisible (v.get() == visible);
        if(v.get()==visible)v->refreshDetector();
    }
    for (std::size_t n = 0; n < keyList.size(); ++n)
    {
        keyList[n].button->setToggleState ((int) n == key, juce::dontSendNotification);
        const bool sameUnit=saturation?keyList[n].saturationSlot==keyList[(std::size_t)key].saturationSlot:(int)n==key;
        keyList[n].button->setVisible(!compactEq()&&(!mixed||sameUnit));
    }
    unitChoice.setVisible ((compactEq() && keyList.size() > 1)||(serviceCover()&&shownUnits.size()>1));
    for (auto* c : std::initializer_list<juce::Component*> { previous.get(), next.get(), &bandChoice }) c->setVisible (compactEq());
    line.setVisible (! compactEq());
    patchBay->setVisible(visible!=nullptr&&serviceCover()&&!compactEq());
    saturationGraph->setVisible(saturationGlobal());
    if(saturationGlobal())saturationGraph->refresh(shown,std::uint8_t(keyList[std::size_t(key)].saturationSlot));
    if(saturationGraph->getPeer())saturationGraph->toFront(false);
    resized();
    unitChoice.setSelectedId (key + 1, juce::dontSendNotification);
    bandChoice.setSelectedId (routesPage() + 1, juce::dontSendNotification);
    resized();
    repaint();
    if (const auto h = shownHeight(); h != heightShown)
    {
        heightShown = h;
        if (onHeight)
            onHeight();
    }
    if(onPageChanged)onPageChanged();
}

bool AllControlsPanel::serviceCover() const {return std::any_of(shownUnits.begin(),shownUnits.end(),[](const auto& u){return u.module==NativeModule::saturation;});}
bool AllControlsPanel::saturationGlobal() const{return juce::isPositiveAndBelow(key,int(keyList.size()))&&keyList[std::size_t(key)].saturationSlot>=0&&keyList[std::size_t(key)].button->getButtonText()=="GLOBAL";}
void AllControlsPanel::retireGraph(){if(saturationGraph)saturationGraph->retire();}
void AllControlsPanel::visibilityChanged()
{
    const juce::Component::SafePointer<AllControlsPanel> alive(this);
    if(!isVisible())for(auto* knob:byIndex){if(knob)knob->closeEntry();if(!alive)return;}
}

bool AllControlsPanel::compactEq() const
{
    return juce::isPositiveAndBelow (key, (int) keyList.size()) && keyList[(std::size_t) key].eqSlot >= 0;
}

void AllControlsPanel::selectEqPage (int page)
{
    if (! compactEq() || ! juce::isPositiveAndBelow (page, (int) keyList[(std::size_t) key].pages.size())) return;
    const auto scope = host.house.frame ? host.house.frame() : nullptr;
    keyList[(std::size_t) key].page = page; showShown();
    if ((page < 8||page>=10) && onBandSelected && ! (host.house.closing && host.house.closing()))
        onBandSelected ((std::size_t) keyList[(std::size_t) key].eqSlot, (std::size_t) (page>=18?page-18:page>=10?page-10:page));
}

void AllControlsPanel::rereadRoutes()
{
    for (std::size_t i = 252; i < 572; ++i)
        if (auto* k = byIndex[i])
            k->reread();
}

std::vector<AllControlsPanel::Unit> panelUnits (const RackInstance& rack)
{
    std::vector<AllControlsPanel::Unit> units;
    for (const auto& u : rack.design.units)
        if (const auto* b = rackUnitBinding (rack, u.id))
            units.push_back ({ u.module, b->slot, u.algorithmRevision });
    return units;
}
} // namespace airack::studio
