#include "LibraryUnit.h"
#include "ProgrammerLook.h"
#include <algorithm>
#include <map>
#include <set>
namespace airack::studio {
void clearRackLibraryFilters(RackLibraryBrowserState& state){state.query.text.clear();state.query.role.reset();state.query.tags.clear();}
namespace {
void identify(juce::Component& c,const juce::String& id,const juce::String& name){c.setComponentID(id);c.setName(name);}
juce::String statusText(RackLibraryStatus s){
    constexpr const char* names[]{"Ready","Unavailable","Invalid library data","Incompatible with this AIR release","Release identity conflict","Library byte limit reached","Library count limit reached","Cancelled","Request timed out","Network unavailable","Service rate limit; retry later","Library storage unavailable","This version is in use. Close projects using it, then retry."};
    return names[int(s)];
}
// What the status line says to a musician; the exact technical status stays in the tooltip's source data, not here.
juce::String plainStatus(RackLibraryStatus s,const juce::String& message){
    switch(s){
    case RackLibraryStatus::unavailable:case RackLibraryStatus::timeout:case RackLibraryStatus::networkFailure:
        return "Can't reach the rack library right now. The racks built into air still work; press REFRESH when you are online.";
    case RackLibraryStatus::rateLimited:
        return "The rack library is busy. Try again in a few minutes.";
    default:{auto text=statusText(s);if(s!=RackLibraryStatus::busy&&message.isNotEmpty()&&message!=text)text+=": "+message;return text;}
    }
}
int versionCompare(const juce::String& a,const juce::String& b){auto x=juce::StringArray::fromTokens(a,".",""),y=juce::StringArray::fromTokens(b,".","");for(int n=0;n<3;++n)if(x[n].getLargeIntValue()!=y[n].getLargeIntValue())return x[n].getLargeIntValue()<y[n].getLargeIntValue()?-1:1;return 0;}
juce::String keyFor(const RackInventoryRow& row,unsigned edge){return row.package?row.package->id+":"+row.package->version+":"+row.package->sha256+":"+juce::String(edge):row.metadata.design.toString()+":"+juce::String(row.metadata.revision)+":"+row.metadata.canonicalSha256+":"+juce::String(edge);}
}
struct LibraryUnit::Browser final : juce::ListBoxModel {
    LibraryUnit& owner;InstrumentProcessor& processor;const Model& model;Host host;
    ProgrammerLook look;
    Lettering caps,mono;
    RackLibraryBrowserState state;
    std::shared_ptr<RackLibraryRuntime> runtime;
    RackInventoryPtr inventory;
    std::shared_ptr<const RackLibraryClientView> clientView;
    RackLibraryPlacementPreparationPtr preparation;
    std::vector<RackInventoryRow> rows;
    std::optional<RackInventoryRow> selectedDisplay;
    std::optional<RackPackageRef> pendingRelease;
    std::set<juce::String> localIdentities;
    std::vector<std::size_t> matches;
    std::vector<RackTag> tags;
    std::vector<std::pair<RackDestination,juce::String>> placeOptions;
    SoundDocument sound;
    juce::Uuid selectedRack=juce::Uuid::null();
    std::optional<RackDesign> localDesign;
    std::optional<RackPackageRef> localPackage;
    RackLibraryStatus inventoryStatus=RackLibraryStatus::busy;
    std::uint64_t job=0;
    bool openPending=false,filterOpen=false,confirmRemove=false,removing=false,updating=false,tagOverflow=false;
    int columns=2,keyboardColumn=0;
    juce::String message,reviewText;
    KeyFocus<juce::TextButton> all{"ALL"},installed{"INSTALLED"},updates{"UPDATES"},filter{"FILTER"},refreshKey{"REFRESH"},back{"BACK"},cancelKey{"CANCEL"},clear{"CLEAR FILTERS"};
    KeyFocus<juce::TextButton> add{"ADD"},install{"INSTALL"},remove{"UNINSTALL"},folder{"OPEN LIBRARY FOLDER"},reuse{"REUSE: OFF"};
    juce::TextEditor search,allocation,description,details;
    juce::ComboBox role,destination,releases;
    std::vector<RackPackageRef> managedReleases;
    juce::Label status,difference,criteria,empty,tagOverflowCue;
    struct Gallery final : juce::ListBox {
        using juce::ListBox::ListBox;
        std::function<bool(const juce::KeyPress&)> onKey;
        bool keyPressed(const juce::KeyPress& key) override{return onKey&&onKey(key)?true:juce::ListBox::keyPressed(key);}
    };
    Gallery gallery{"Rack Gallery",this};
    juce::Viewport filterView;
    juce::Component filterBody;
    std::vector<std::unique_ptr<KeyFocus<juce::TextButton>>> tagKeys;
    std::map<juce::String,juce::Image> images;
    std::uint64_t imageBytes=0;
    std::vector<juce::String> wantedImages;
    juce::Rectangle<int> content,preview;
    Browser(LibraryUnit& o,InstrumentProcessor& p,const Model& m,Host h):owner(o),processor(p),model(m),host(std::move(h)),look(m),caps(m.face),mono(m.screenFace),runtime(p.rackLibraryRuntime()){
        owner.setLookAndFeel(&look);owner.setWantsKeyboardFocus(true);owner.setFocusContainerType(juce::Component::FocusContainerType::focusContainer);identify(owner,"library.panel","AIR rack library");
        const auto key=[&](juce::Button& b,const char* id){identify(b,id,b.getButtonText());owner.addAndMakeVisible(b);};
        key(all,"library.all");key(installed,"library.installed");key(updates,"library.updates");key(filter,"library.filter");key(refreshKey,"library.refresh");key(back,"library.back");key(cancelKey,"library.cancel");key(clear,"library.clear");key(add,"library.apply");key(install,"library.install");key(remove,"library.uninstall");key(folder,"library.folder");key(reuse,"library.reuse");
        search.setTextToShowWhenEmpty("Name, maker, purpose or tags",juce::Colour(0xff81918a));identify(search,"library.search","Search rack library");owner.addAndMakeVisible(search);
        identify(role,"library.role","Rack type");role.addItem("TYPE: ALL",1);role.addItem("TYPE: SYNTH",2);role.addItem("TYPE: FX",3);role.setSelectedId(1);owner.addAndMakeVisible(role);
        identify(allocation,"library.allocation","Exact banks: local unit:physical bank, blank for automatic");allocation.setTextToShowWhenEmpty("AUTO BANKS (unit:bank)",juce::Colour(0xff81918a));owner.addAndMakeVisible(allocation);
        identify(destination,"library.placement","Place as destination");owner.addAndMakeVisible(destination);
        identify(releases,"library.releases","Secondary management: exact installed release to uninstall");owner.addChildComponent(releases);releases.onChange=[this]{confirmRemove=false;updateDetail();};
        identify(description,"library.description","Rack description and release details");description.setReadOnly(true);description.setMultiLine(true,true);description.setScrollbarsShown(true);description.setFont(mono.fontForCap(20));owner.addAndMakeVisible(description);identify(details,"library.details","Exact rack release metadata");details.setReadOnly(true);details.setMultiLine(true,true);details.setScrollbarsShown(true);details.setFont(mono.fontForCap(19));owner.addAndMakeVisible(details);search.setFont(mono.fontForCap(20));allocation.setFont(mono.fontForCap(19));
        identify(empty,"library.empty","No matching racks; clear filters while preserving collection");empty.setJustificationType(juce::Justification::centred);empty.setFont(juce::Font(juce::FontOptions(model.screenFace).withHeight(25)));owner.addChildComponent(empty);identify(status,"library.status","Library status");identify(difference,"library.diff","Required banks and placement review");identify(criteria,"library.criteria","Active type and tags");
        for(auto* c:{&status,&difference,&criteria}){owner.addAndMakeVisible(*c);c->setColour(juce::Label::textColourId,juce::Colour(0xffc8d3cb));c->setFont(mono.fontForCap(18));}
        identify(gallery,"library.gallery","Rack Gallery; select a rack to open its detail");gallery.setRowHeight(374);gallery.setColour(juce::ListBox::backgroundColourId,juce::Colour(0xff090b0c));gallery.setColour(juce::ListBox::outlineColourId,juce::Colour(0xff4c5352));gallery.setOutlineThickness(1);gallery.setMultipleSelectionEnabled(false);owner.addAndMakeVisible(gallery);
        identify(filterView,"library.filters","Role and all selected tag filters");filterView.setViewedComponent(&filterBody,false);filterView.setScrollBarsShown(true,false);owner.addChildComponent(filterView);
        identify(tagOverflowCue,"library.tag-overflow","Tag filter scope exceeds 128; narrow Search, Type or Collection");tagOverflowCue.setText("TAG FILTER SCOPE EXCEEDS 128\nNarrow Search, Type or Collection to use tags.\nSelected tags still apply.",juce::dontSendNotification);tagOverflowCue.setFont(mono.fontForCap(20));tagOverflowCue.setColour(juce::Label::textColourId,juce::Colour(0xffc8d3cb));tagOverflowCue.setJustificationType(juce::Justification::topLeft);filterBody.addChildComponent(tagOverflowCue);
        const auto collection=[this](RackLibraryCollection c){state.query.collection=c;queryChanged();};all.onClick=[collection]{collection(RackLibraryCollection::all);};installed.onClick=[collection]{collection(RackLibraryCollection::installed);};updates.onClick=[collection]{collection(RackLibraryCollection::updates);};
        filter.onClick=[this]{filterOpen=!filterOpen;layout();};clear.onClick=[this]{clearRackLibraryFilters(state);search.setText({},false);role.setSelectedId(1,juce::dontSendNotification);queryChanged();};search.onTextChange=[this]{state.query.text=search.getText();queryChanged();};
        for(auto* editor:{&search,&allocation,&description,&details})editor->onEscapeKey=[this]{owner.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));};
        role.onChange=[this]{const auto id=role.getSelectedId();state.query.role=id==1?std::optional<RackRole>{}:std::optional(id==2?RackRole::synth:RackRole::fx);queryChanged();};
        back.onClick=[this]{goBack();};cancelKey.onClick=[this]{const auto frame=host.frame?host.frame():nullptr;if(host.closing&&host.closing())return;auto done=owner.onDone;owner.cancel();if(done)done();};refreshKey.onClick=[this]{refreshCatalog();};add.onClick=[this]{apply();};install.onClick=[this]{startInstall();};remove.onClick=[this]{uninstall();};
        reuse.onClick=[this]{reuse.setToggleState(!reuse.getToggleState(),juce::dontSendNotification);reuse.setButtonText(reuse.getToggleState()?"REUSE: ON":"REUSE: OFF");review();};allocation.onTextChange=[this]{review();};destination.onChange=[this]{review();};folder.onClick=[this]{job=runtime->client->revealLibraryFolder();if(!job)message="Library is busy; finish the current job first";refreshStatus();};
        gallery.onKey=[this](const juce::KeyPress& key){if(key.getKeyCode()==juce::KeyPress::leftKey||key.getKeyCode()==juce::KeyPress::rightKey){keyboardColumn=key.getKeyCode()==juce::KeyPress::rightKey?columns-1:0;gallery.repaint();return true;}return false;};preparation=runtime->client->createPlacementPreparation();all.setToggleState(true,juce::dontSendNotification);
    }
    ~Browser(){runtime->client->cancelPlacement(preparation);owner.setLookAndFeel(nullptr);}
    const RackInventoryRow* chosen() const{for(const auto& row:rows)if(row.metadata.design==state.design&&row.metadata.revision==state.revision&&row.metadata.canonicalSha256==state.canonicalSha256&&row.package==state.release)return &row;return selectedDisplay?&*selectedDisplay:nullptr;}
    const RackRemoteRelease* latest(const RackInventoryRow& row) const{const RackRemoteRelease* found=nullptr;if(clientView)for(const auto& r:clientView->releases)if(row.package&&r.ref.id==row.package->id&&r.compatible&&(!found||versionCompare(found->ref.version,r.ref.version)<0))found=&r;return found;}
    const RackRemoteRelease* exactRemote(const RackInventoryRow& row) const{if(clientView&&row.package)for(const auto& r:clientView->releases)if(r.ref==*row.package)return &r;return nullptr;}
    juce::String downloads(const RackInventoryRow& row) const{if(clientView&&row.package)for(const auto& c:clientView->counts)if(c.id==row.package->id&&c.value)return juce::String(*c.value)+(clientView->countsStale?" (stale)":"");return "Unknown";}
    bool localFor(const RackInventoryRow& row) const{return localIdentities.contains(keyFor(row,0))||(localDesign&&localDesign->id==row.metadata.design&&localDesign->revision==row.metadata.revision&&localPackage==row.package&&state.canonicalSha256==row.metadata.canonicalSha256);}
    bool resolveLocal(const RackInventoryRow& row,RackDesign& design,std::optional<RackPackageRef>& package) const{
        if(!inventory||!resolveInventoryDesign(*inventory,row.metadata.design,row.metadata.revision,row.metadata.canonicalSha256,design,package))return false;
        if(package==row.package)return true;
        if(sound.personalities)for(const auto& rack:sound.personalities->instances)if(rack.design==design&&rack.package==row.package){package=row.package;return true;}
        return false;
    }
    juce::String availability(const RackInventoryRow& row) const{if(row.builtIn)return "INCLUDED";if(row.updateAvailable)return "UPDATE AVAILABLE";if(row.installed)return "INSTALLED";if(const auto* r=exactRemote(row);r&&!r->compatible)return "INCOMPATIBLE";if(localFor(row))return "PROJECT RECIPE";return "AVAILABLE";}
    std::span<const RackTag> knownTags() const{return clientView?std::span<const RackTag>(clientView->tags):std::span<const RackTag>{};}
    juce::String tagText(const RackInventoryRow& row) const{juce::StringArray labels;const auto known=knownTags();for(const auto& id:row.metadata.tags){const auto it=std::find_if(known.begin(),known.end(),[&](const auto& t){return t.id==id;});labels.add(it==known.end()?id:it->label);}return labels.joinIntoString(", ");}
    void merge(){
        rows.clear();localIdentities.clear();if(inventory){rows.assign(inventory->rows().begin(),inventory->rows().end());for(const auto& row:rows)localIdentities.insert(keyFor(row,0));}
        if(clientView)for(const auto& remote:clientView->releases){
            auto local=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.package&&row.package->id==remote.ref.id&&row.installed;});
            if(local!=rows.end()){if(remote.compatible&&versionCompare(local->package->version,remote.ref.version)<0)local->updateAvailable=true;continue;}
            auto existing=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.package&&row.package->id==remote.ref.id&&!row.installed;});
            if(existing==rows.end())rows.push_back({remote.metadata,remote.ref,false,false,false});else if(const auto* current=exactRemote(*existing);current&&((remote.compatible&&!current->compatible)||(remote.compatible==current->compatible&&versionCompare(existing->package->version,remote.ref.version)<0))){existing->metadata=remote.metadata;existing->package=remote.ref;}
        }
        if(inventory&&sound.personalities)for(const auto& live:sound.personalities->instances){
            if(!live.package||std::any_of(rows.begin(),rows.end(),[&](const auto& row){return row.metadata.design==live.design.id&&row.metadata.revision==live.design.revision&&row.package==live.package;}))continue;
            for(const auto& local:inventory->rows())if(local.metadata.design==live.design.id&&local.metadata.revision==live.design.revision){
                RackDesign definition;std::optional<RackPackageRef> package;
                if(!resolveInventoryDesign(*inventory,local.metadata.design,local.metadata.revision,local.metadata.canonicalSha256,definition,package)||definition!=live.design)continue;
                RackInventoryRow display=local;display.package=live.package;display.builtIn=display.installed=display.updateAvailable=false;
                if(clientView)for(const auto& remote:clientView->releases)if(remote.ref==*live.package&&remote.metadata.canonicalSha256==local.metadata.canonicalSha256){display.metadata=remote.metadata;break;}
                localIdentities.insert(keyFor(display,0));rows.push_back(std::move(display));break;
            }
        }
        if(pendingRelease&&clientView&&!clientView->busy&&clientView->status==RackLibraryStatus::ready)
            for(const auto& row:rows)if(row.installed&&row.package==pendingRelease){state.design=row.metadata.design;state.revision=row.metadata.revision;state.canonicalSha256=row.metadata.canonicalSha256;state.release=row.package;selectedDisplay=row;pendingRelease.reset();break;}
        if(selectedDisplay){bool found=false;for(const auto& row:rows)if(row.metadata.design==state.design&&row.metadata.revision==state.revision&&row.metadata.canonicalSha256==state.canonicalSha256&&row.package==state.release){selectedDisplay=row;found=true;break;}if(!found){selectedDisplay->installed=false;selectedDisplay->updateAvailable=false;}}
        queryChanged(false);resolveSelection();
    }
    void rebuildTagKeys(){tagKeys.clear();for(const auto& tag:tags){auto b=std::make_unique<KeyFocus<juce::TextButton>>(tag.label.toUpperCase());ProgrammerLook::setCompactButton(*b);identify(*b,"library.tag."+tag.id,"Tag "+tag.label+"; all selected tags must match");filterBody.addAndMakeVisible(*b);b->onClick=[this,id=tag.id]{if(state.query.tags.contains(id))state.query.tags.removeString(id);else state.query.tags.add(id);queryChanged();};tagKeys.push_back(std::move(b));}}
    void queryChanged(bool resetScroll=true){
        if(resetScroll)state.galleryScroll=0;
        // Available tags follow the same scope query, before selected-tag AND filtering.
        auto scope=state.query;scope.tags.clear();const auto known=knownTags();std::vector<RackTag> nextTags;tagOverflow=false;
        for(const auto n:queryRackRows(rows,scope,known)){
            for(const auto& id:rows[n].metadata.tags){
                if(std::any_of(nextTags.begin(),nextTags.end(),[&](const auto& tag){return tag.id==id;}))continue;
                if(nextTags.size()==rackPackageLimits::tagDictionaryCount){tagOverflow=true;nextTags.clear();break;}
                const auto label=std::find_if(known.begin(),known.end(),[&](const auto& tag){return tag.id==id;});nextTags.push_back({id,label==known.end()?id:label->label});
            }
            if(tagOverflow)break;
        }
        if(tags.size()!=nextTags.size()||!std::equal(tags.begin(),tags.end(),nextTags.begin(),[](const auto& a,const auto& b){return a.id==b.id&&a.label==b.label;})){tags=std::move(nextTags);rebuildTagKeys();}
        matches=queryRackRows(rows,state.query,known);
        all.setToggleState(state.query.collection==RackLibraryCollection::all,juce::dontSendNotification);installed.setToggleState(state.query.collection==RackLibraryCollection::installed,juce::dontSendNotification);updates.setToggleState(state.query.collection==RackLibraryCollection::updates,juce::dontSendNotification);
        for(std::size_t n=0;n<tagKeys.size();++n)tagKeys[n]->setToggleState(state.query.tags.contains(tags[n].id),juce::dontSendNotification);
        criteria.setText("TYPE: "+(state.query.role?(*state.query.role==RackRole::synth?juce::String("SYNTH"):"FX"):"ALL")+"   TAGS: "+(state.query.tags.isEmpty()?juce::String("ALL"):state.query.tags.joinIntoString(", ")),juce::dontSendNotification);
        gallery.updateContent();gallery.getViewport()->setViewPosition(0,state.galleryScroll);layout();owner.repaint();
    }
    int getNumRows() override{return int((matches.size()+std::size_t(columns)-1)/std::size_t(columns));}
    void paintListBoxItem(int row,juce::Graphics& g,int width,int height,bool focusedRow) override{
        for(int col=0;col<columns;++col){const auto n=std::size_t(row*columns+col);if(n>=matches.size())break;const auto& item=rows[matches[n]];auto cell=juce::Rectangle<int>(col*width/columns,0,width/columns,height).reduced(14,10);
            const bool selected=item.metadata.design==state.design&&item.metadata.revision==state.revision&&item.metadata.canonicalSha256==state.canonicalSha256&&item.package==state.release;
            g.setColour(juce::Colour(selected?0xff142022:0xff0a0c0d));g.fillRect(cell);g.setColour(selected?accent:juce::Colour(0xff3b4142));g.drawRect(cell,selected?2:1);if(focusedRow&&col==keyboardColumn&&gallery.hasKeyboardFocus(true)){g.setColour(juce::Colour(0xffd7dfda));g.drawRect(cell.reduced(4),1);}
            const auto well=cell.removeFromTop(138).reduced(10,8);paintImage(g,item,well,512);cell.reduce(12,0);
            g.setColour(juce::Colour(0xffd7dfda));g.setFont(caps.fontForCap(24));g.drawFittedText(item.metadata.name.toUpperCase(),cell.removeFromTop(44),juce::Justification::centredLeft,1,.8f);
            g.setFont(mono.fontForCap(19));g.drawFittedText(item.metadata.maker.toUpperCase()+"  |  "+(item.metadata.role==RackRole::synth?"SYNTH":"FX"),cell.removeFromTop(40),juce::Justification::centredLeft,1,.8f);
            g.setColour(juce::Colour(0xffa8b8b0));g.drawFittedText(item.metadata.purpose,cell.removeFromTop(62),juce::Justification::topLeft,2,.8f);g.setFont(mono.fontForCap(18));g.drawFittedText("TAGS: "+tagText(item),cell.removeFromTop(38),juce::Justification::centredLeft,1,.8f);
            g.setColour(juce::Colour(0xffd7dfda));g.drawFittedText(availability(item)+"    DOWNLOADS: "+downloads(item),cell,juce::Justification::centredLeft,1,.8f);
        }
    }
    void listBoxItemClicked(int row,const juce::MouseEvent& e) override{open(row*columns+std::min(columns-1,e.x*columns/std::max(1,gallery.getViewport()->getViewWidth())));}
    void returnKeyPressed(int row) override{open(row*columns+keyboardColumn);}
    void listWasScrolled() override{if(!updating){state.galleryScroll=gallery.getViewport()->getViewPositionY();updateImages();}}
    void open(int n){if(!juce::isPositiveAndBelow(n,int(matches.size())))return;const auto& row=rows[matches[std::size_t(n)]];selectedDisplay=row;state.design=row.metadata.design;state.revision=row.metadata.revision;state.canonicalSha256=row.metadata.canonicalSha256;state.release=row.package;state.galleryScroll=gallery.getViewport()->getViewPositionY();state.detailOpen=true;filterOpen=false;confirmRemove=false;removing=false;runtime->client->cancelPlacement(preparation);message.clear();resolveSelection();layout();back.grabKeyboardFocus();owner.repaint();}
    void goBack(){if(filterOpen){filterOpen=false;layout();return;}state.detailOpen=false;confirmRemove=false;runtime->client->cancelPlacement(preparation);localDesign.reset();layout();gallery.getViewport()->setViewPosition(0,state.galleryScroll);gallery.grabKeyboardFocus();owner.repaint();}
    void resolveSelection(){
        localDesign.reset();localPackage.reset();if(!state.detailOpen||confirmRemove||removing)return;
        if(const auto* row=chosen();row&&inventory){RackDesign d;std::optional<RackPackageRef> ref;if(resolveLocal(*row,d,ref)){localDesign=std::move(d);localPackage=ref;std::vector<RackPackageRef> refs;if(ref&&row->installed)refs.push_back(*ref);runtime->client->requestPlacement(preparation,inventory,refs);}}
        populatePlaces();review();updateDetail();
    }
    std::vector<std::pair<RackDestination,juce::String>> places() const{
        std::vector<std::pair<RackDestination,juce::String>> out;if(!localDesign)return out;
        if(localDesign->role==RackRole::synth){for(int n=0;n<=sound.layout.synthCount;++n)out.push_back({{RackRole::synth,masterChain,std::uint8_t(n)},"SYNTH "+juce::String(n+1)+(n==0?", FIRST":", AFTER "+unitName(sound.layout.synthOrder[std::size_t(n-1)],true))});}
        else{const auto chain=[&](std::uint8_t c,const juce::String& label){for(int n=0;n<=sound.layout.effectCount[c];++n)out.push_back({{RackRole::fx,c,std::uint8_t(n)},label+(n==0?", FIRST":", AFTER "+unitName(sound.layout.effectOrder[c][std::size_t(n-1)],false))});};chain(masterChain,"SHARED MASTER");for(int n=0;n<sound.layout.synthCount;++n)chain(sound.layout.synthOrder[std::size_t(n)],"SYNTH "+juce::String(int(sound.layout.synthOrder[std::size_t(n)])+1)+" INSERT");}return out;
    }
    juce::String unitName(std::uint8_t slot,bool synth) const{
        if(sound.personalities)for(const auto& rack:sound.personalities->instances)for(const auto& unit:rack.design.units)
            if(const auto* binding=rackUnitBinding(rack,unit.id);binding&&binding->slot==slot&&isSourceModule(unit.module)==synth)return rack.design.name.toUpperCase();
        return synth?moduleDisplayName(sourceModule(sound,slot)).toUpperCase():effectDisplayName(sound.layout.effects[slot].kind).toUpperCase();
    }
    void populatePlaces(){
        std::optional<RackDestination> previous;const auto was=destination.getSelectedId()-1;
        if(juce::isPositiveAndBelow(was,int(placeOptions.size())))previous=placeOptions[std::size_t(was)].first;
        destination.clear(juce::dontSendNotification);placeOptions=places();int selected=0;
        for(std::size_t n=0;n<placeOptions.size();++n){const auto& [place,label]=placeOptions[n];destination.addItem(label,int(n+1));if(previous&&previous->role==place.role&&previous->chain==place.chain&&previous->position==place.position)selected=int(n+1);}
        if(!placeOptions.empty())destination.setSelectedId(selected?selected:(localDesign->role==RackRole::synth?int(placeOptions.size()):int(sound.layout.effectCount[masterChain])+1),juce::dontSendNotification);
    }
    RackReviewRequest request() const{RackReviewRequest r;r.localDesign=localDesign;r.package=localPackage;r.selected=selectedRack;r.allocation=allocation.getText();r.reuse=reuse.getToggleState();const auto list=places();const auto n=destination.getSelectedId()-1;if(juce::isPositiveAndBelow(n,int(list.size()))){r.chain=list[std::size_t(n)].first.chain;r.position=list[std::size_t(n)].first.position;}return r;}
    void review(){
        if(localDesign){
            const auto reviewed=reviewRack(processor,request());reviewText=reviewed.difference;
            int synths=0,effects=0;for(const auto& unit:localDesign->units)++(isSourceModule(unit.module)?synths:effects);
            difference.setText("NEEDS "+juce::String(synths)+" SYNTH / "+juce::String(effects)+" FX BANKS | "+reviewed.state,juce::dontSendNotification);
            difference.setTooltip(reviewText);
        }else{reviewText.clear();difference.setText("Install first, then Add. Browsing does not change the sound.",juce::dontSendNotification);}
    }
    std::optional<RackPackageRef> removalTarget() const{const auto n=releases.getSelectedId()-1;return juce::isPositiveAndBelow(n,int(managedReleases.size()))?std::optional(managedReleases[std::size_t(n)]):std::nullopt;}
    void updateManagedReleases(const RackInventoryRow& row){
        std::vector<RackPackageRef> next;if(row.package&&clientView)for(const auto& ref:clientView->committed)if(ref.id==row.package->id)next.push_back(ref);
        std::sort(next.begin(),next.end(),[](const auto& a,const auto& b){return versionCompare(a.version,b.version)>0;});
        if(next!=managedReleases){const auto previous=removalTarget();managedReleases=std::move(next);releases.clear(juce::dontSendNotification);int selected=0;for(std::size_t n=0;n<managedReleases.size();++n){const auto& ref=managedReleases[n];releases.addItem("MANAGE RELEASE: "+ref.version,int(n+1));if(ref==(previous?previous:row.package))selected=int(n+1);}if(!managedReleases.empty())releases.setSelectedId(selected?selected:1,juce::dontSendNotification);confirmRemove=false;}
    }
    void updateDetail(){
        const auto* row=chosen();if(!row)return;updateManagedReleases(*row);const auto target=removalTarget();const auto* remote=exactRemote(*row);const auto* newer=latest(*row);
        const auto installedVersion=row->builtIn?juce::String("Included with AIR"):row->installed&&row->package?row->package->version:localFor(*row)&&row->package?juce::String("Not active; exact artwork unverified"):juce::String("Not installed");
        const auto line=[](const char* label,const juce::String& value){return juce::String(label).paddedRight(' ',17)+value+"\n";};
        auto metadata=line("TYPE:",row->metadata.role==RackRole::synth?"SYNTH":"FX")+line("TAGS:",tagText(*row))+line("DOWNLOADS:",downloads(*row))
            +line("COMPATIBILITY:",remote?(remote->compatible?juce::String("Compatible"):remote->incompatibility):localFor(*row)?juce::String("Compatible native AIR recipe"):"Unknown; refresh catalog")
            +line("STATUS:",availability(*row))+line("EXACT VERSION:",row->package?row->package->version:juce::String(row->metadata.revision))
            +line("INSTALLED:",installedVersion)+line("LATEST:",newer?newer->ref.version:row->builtIn?juce::String("Included with AIR"):"Unknown")
            +line("DOWNLOAD SIZE:",remote?juce::File::descriptionOfSizeInBytes(juce::int64(remote->archiveBytes)):row->builtIn?juce::String("Included; no download"):"Unknown; catalog unavailable");
        if(row->package&&clientView)for(const auto& other:clientView->releases)if(other.ref.id==row->package->id&&!other.compatible&&versionCompare(other.ref.version,row->package->version)>=0)metadata+=line("INCOMPATIBLE:",other.ref.version+" - "+other.incompatibility);
        auto fullDescription=row->metadata.name+"\nMAKER: "+row->metadata.maker+"\n\n"+row->metadata.purpose
            +(row->metadata.description.isNotEmpty()&&row->metadata.description!=row->metadata.purpose?"\n\n"+row->metadata.description:juce::String{})
            +(reviewText.isNotEmpty()?"\n\nPLACEMENT REVIEW\n"+reviewText:juce::String{});
        if(confirmRemove&&target)fullDescription="REMOVE "+target->id+" "+target->version+"?\nSaved racks remain and may need this exact artwork restored.\nPress CONFIRM REMOVE, or Back to cancel.\n\n"+fullDescription;
        if(description.getText()!=fullDescription)description.setText(fullDescription,false);
        if(details.getText()!=metadata)details.setText(metadata,false);
        const auto held=runtime->client->placement(preparation);const bool prepared=!confirmRemove&&!removing&&localDesign&&held&&held->root==inventory&&runtime->client->placementCurrent(preparation,*held);
        add.setEnabled(prepared);install.setButtonText(localDesign&&!row->installed&&!row->builtIn&&row->package?"RESTORE":row->updateAvailable?"UPDATE":"INSTALL");
        const bool downloadable=row->package&&!row->builtIn&&((remote&&remote->compatible)||row->updateAvailable);
        install.setVisible(state.detailOpen&&downloadable&&(!row->installed||row->updateAvailable));install.setEnabled(!clientView||!clientView->busy);
        releases.setVisible(state.detailOpen&&!filterOpen&&!managedReleases.empty());releases.setEnabled(!confirmRemove&&(!clientView||!clientView->busy));remove.setVisible(state.detailOpen&&!row->builtIn&&target.has_value());remove.setButtonText(confirmRemove?"CONFIRM REMOVE":"UNINSTALL");remove.setEnabled(!clientView||!clientView->busy);
    }
    void refreshCatalog(){message.clear();processor.retryAvailableRackInventory();job=runtime->client->refresh();}
    void startInstall(){const auto* row=chosen();if(!row||!row->package||row->builtIn)return;message.clear();if(localDesign&&!row->installed){job=runtime->client->restore(*row->package);}else if(row->updateAvailable){if(const auto* next=latest(*row)){pendingRelease=next->ref;job=runtime->client->install(next->ref);}}else{pendingRelease=row->package;job=runtime->client->install(*row->package);}refreshStatus();}
    void uninstall(){const auto* row=chosen();const auto target=removalTarget();if(!row||row->builtIn||!target)return;if(!confirmRemove){confirmRemove=true;runtime->client->cancelPlacement(preparation);updateDetail();description.setCaretPosition(0);description.scrollEditorToPositionCaret(0,0);message="Remove "+target->id+" "+target->version+"? Saved racks remain; artwork may need Restore.";refreshStatus();return;}runtime->client->cancelPlacement(preparation);removing=true;job=runtime->client->remove(*target);confirmRemove=false;message.clear();refreshStatus();}
    void apply(){
        const auto frame=host.frame?host.frame():nullptr;if(host.closing&&host.closing())return;const auto* row=chosen();if(!row||!inventory)return;RackDesign d;std::optional<RackPackageRef> ref;
        if(!resolveLocal(*row,d,ref)){message="Selected local recipe is unavailable; refresh";return refreshStatus();}
        const auto held=runtime->client->placement(preparation);if(!held||held->root!=inventory||!runtime->client->placementCurrent(preparation,*held)){message="Preparing exact release; retry Add";return refreshStatus();}
        localDesign=std::move(d);localPackage=ref;const auto reviewed=reviewRack(processor,request());difference.setText(reviewed.difference,juce::dontSendNotification);
        if(reviewed.draft.empty()){message=reviewed.state+": "+reviewed.difference.upToFirstOccurrenceOf("\n",false,false);return refreshStatus();}
        const auto result=applyRackReview(processor,reviewed,preparation,held);message=result.status;if(host.closing&&host.closing())return;const juce::Component::SafePointer<LibraryUnit> safe(&owner);
        if(result.applied&&result.placed&&owner.onPlaced)owner.onPlaced(*result.placed,result.status);if(safe&&result.applied&&owner.onDone)owner.onDone();if(safe)refreshStatus();
    }
    void refreshStatus(){folder.setEnabled(!clientView||!clientView->busy);refreshKey.setEnabled(!clientView||!clientView->busy);juce::String text=message;if(text.isEmpty()&&clientView){text=(clientView->cached?juce::String("CACHED  "):juce::String{});if(clientView->busy)text+="WORKING "+juce::String(clientView->bytesDone)+" / "+juce::String(clientView->bytesTotal)+" bytes; CANCEL stops transfer";else text+=clientView->offline&&clientView->status==RackLibraryStatus::ready?juce::String("You are offline; showing the racks saved on this computer."):plainStatus(clientView->status,clientView->message);}if(!inventory)text+="  LOCAL: "+statusText(inventoryStatus);status.setText(text,juce::dontSendNotification);status.setTooltip(text);}
    void refresh(){
        if(updating)return;updating=true;if(openPending&&owner.isVisible()&&!runtime->client->view()->busy){job=runtime->client->openLibrary();openPending=false;}SoundDocument d;std::uint64_t edit=0,reset=0;const bool soundChanged=processor.trySoundSnapshot(d,edit,reset)&&d!=sound;if(soundChanged)sound=std::move(d);
        RackInventoryPtr captured;const bool ready=processor.captureAvailableRackInventory(captured,&inventoryStatus);const bool inventoryChanged=ready&&captured!=inventory;if(inventoryChanged)inventory=std::move(captured);
        const auto next=runtime->client->view();const bool changed=!clientView||next!=clientView;clientView=next;
        if(inventoryChanged||changed)merge();else if(soundChanged&&state.detailOpen){populatePlaces();review();}
        if(removing&&clientView&&!clientView->busy&&clientView->job==job){removing=false;resolveSelection();}if(state.detailOpen)updateDetail();updateImages();refreshStatus();updating=false;
    }
    void updateImages(){
        wantedImages.clear();std::vector<std::pair<const RackInventoryRow*,unsigned>> wanted;
        if(state.detailOpen){if(const auto* row=chosen())wanted.push_back({row,2048});}
        else if(!filterOpen){const auto first=gallery.getViewport()->getViewPositionY()/gallery.getRowHeight();const auto last=first+gallery.getViewport()->getViewHeight()/gallery.getRowHeight()+1;for(int r=first;r<=last;++r)for(int c=0;c<columns;++c){const auto n=r*columns+c;if(juce::isPositiveAndBelow(n,int(matches.size())))wanted.push_back({&rows[matches[std::size_t(n)]],512});}}
        for(const auto& [row,edge]:wanted){
            const auto key=keyFor(*row,edge);wantedImages.push_back(key);std::shared_ptr<const RackLibraryThumbnail> image;
            if(row->package){runtime->client->requestThumbnail(*row->package,edge);image=runtime->client->thumbnail(*row->package,edge);}
            else if(row->builtIn){runtime->client->requestBuiltInThumbnail(row->metadata,edge);image=runtime->client->builtInThumbnail(row->metadata,edge);}
            if(image&&image->status==RackLibraryStatus::ready&&image->image.isValid())images[key]=image->image;
        }
        std::erase_if(images,[&](const auto& entry){return std::find(wantedImages.begin(),wantedImages.end(),entry.first)==wantedImages.end();});imageBytes=0;
        for(auto it=images.begin();it!=images.end();){const auto bytes=std::uint64_t(it->second.getWidth())*std::uint64_t(it->second.getHeight())*4;if(bytes>64ull*1024*1024-imageBytes)it=images.erase(it);else{imageBytes+=bytes;++it;}}gallery.repaint();owner.repaint();
    }
    void paintImage(juce::Graphics& g,const RackInventoryRow& row,juce::Rectangle<int> well,unsigned edge){g.setColour(juce::Colour(0xff030405));g.fillRect(well);const auto found=images.find(keyFor(row,edge));if(found!=images.end()){g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);g.drawImageWithin(found->second,well.getX()+4,well.getY()+4,well.getWidth()-8,well.getHeight()-8,juce::RectanglePlacement::centred,false);}else{g.setColour(juce::Colour(0xff7d8b87));g.setFont(mono.fontForCap(18));g.drawFittedText("AIR  /  PREVIEW UNAVAILABLE\nRack details and local Add remain available",well.reduced(12),juce::Justification::centred,2);}g.setColour(juce::Colour(0xff3a4141));g.drawRect(well);}
    void layout(){
        auto bounds=owner.getLocalBounds().reduced(38,12);const bool narrow=owner.getWidth()<1000;
        auto header=bounds.removeFromTop(narrow?166:94);const auto width=header.getWidth();
        const int keyWidth=narrow?width/7:std::max(68,std::min(106,(width-540)/7));int x=header.getRight()-7*keyWidth;
        const int keyStart=x;
        for(auto* b:{&all,&installed,&updates,&filter,&refreshKey,&back,&cancelKey}){b->setBounds(x,header.getY()+(narrow?70:0),keyWidth,90);x+=keyWidth;}
        search.setBounds(header.getX()+190,header.getY()+18,std::max(140,narrow?width-210:keyStart-header.getX()-210),38);
        auto footer=bounds.removeFromBottom(narrow&&state.detailOpen?180:116);status.setBounds(footer.removeFromBottom(28));criteria.setBounds(bounds.removeFromTop(27));content=bounds.reduced(0,4);
        const auto newColumns=owner.getWidth()>=1000?2:1;if(columns!=newColumns){columns=newColumns;gallery.updateContent();}gallery.setBounds(content);gallery.setVisible(!state.detailOpen&&!filterOpen&&!matches.empty());empty.setBounds(content);empty.setText("NO MATCHING RACKS\nSearch: "+state.query.text+"\n"+criteria.getText()+"\nUse CLEAR FILTERS; your collection stays selected.",juce::dontSendNotification);empty.setVisible(!state.detailOpen&&!filterOpen&&matches.empty());filterView.setBounds(content);filterView.setVisible(filterOpen);
        role.setVisible(filterOpen);role.setBounds(content.getX()+14,content.getY()+8,270,42);int y=64;const auto tagWidth=std::max(180,(content.getWidth()-46)/3);for(std::size_t n=0;n<tagKeys.size();++n)tagKeys[n]->setBounds(14+int(n%3)*tagWidth,y+int(n/3)*48,tagWidth-12,40);filterBody.setSize(content.getWidth()-20,y+int((tagKeys.size()+2)/3)*48+16);
        tagOverflowCue.setVisible(tagOverflow);tagOverflowCue.setBounds(14,y,filterBody.getWidth()-28,140);if(tagOverflow)filterBody.setSize(filterBody.getWidth(),y+156);
        clear.setBounds(footer.getX(),footer.getY(),150,82);clear.setVisible(!state.detailOpen);folder.setBounds(footer.getX()+160,footer.getY(),210,82);folder.setVisible(!state.detailOpen);
        for(auto* c:std::initializer_list<juce::Component*>{&description,&details,&destination,&allocation,&reuse,&add})c->setVisible(state.detailOpen&&!filterOpen);
        if(state.detailOpen){
            preview=content.withHeight(std::min(240,content.getHeight()/2));auto information=content.withTrimmedTop(preview.getHeight()+8);information.removeFromTop(42);
            releases.setBounds(content.getX()+8,preview.getBottom()+48,content.getWidth()-16,34);information.removeFromTop(managedReleases.empty()?0:40);description.setBounds(information.removeFromLeft(information.getWidth()/2).withTrimmedRight(8));details.setBounds(information);
            auto actions=footer;destination.setBounds(actions.removeFromLeft(320).withHeight(36));allocation.setBounds(actions.removeFromLeft(narrow?actions.getWidth():260).reduced(8,0).withHeight(36));
            if(narrow)actions=footer.withTrimmedTop(82);
            reuse.setBounds(actions.removeFromLeft(140).withHeight(narrow?70:82));install.setBounds(actions.removeFromLeft(130).withHeight(narrow?70:82));add.setBounds(actions.removeFromLeft(110).withHeight(narrow?70:82));remove.setBounds(actions.withHeight(narrow?70:82));
            difference.setBounds(footer.getX(),footer.getY()+42,narrow?footer.getWidth():570,40);difference.setVisible(true);updateDetail();
        }
        else{difference.setVisible(false);install.setVisible(false);remove.setVisible(false);releases.setVisible(false);}back.setEnabled(state.detailOpen||filterOpen);updateImages();
    }
    void paint(juce::Graphics& g){look.drawFace(g,owner.getLocalBounds().toFloat());g.setColour(juce::Colour(0xff161a1c));g.fillRect(0,0,26,owner.getHeight());g.fillRect(owner.getWidth()-26,0,26,owner.getHeight());g.setColour(juce::Colour(0xff8c9291));for(const auto x:{13,owner.getWidth()-13})for(const auto y:{25,owner.getHeight()-25}){g.fillEllipse(float(x-6),float(y-6),12,12);g.setColour(juce::Colour(0xff141719));g.drawLine(float(x-3),float(y),float(x+3),float(y),2);g.setColour(juce::Colour(0xff8c9291));}
        g.setColour(juce::Colour(0xffe0e2df));g.setFont(juce::Font(juce::FontOptions(model.face).withHeight(34)));g.drawText("LIBRARY",44,20,170,40,juce::Justification::centredLeft);g.setFont(juce::Font(juce::FontOptions(model.face).withHeight(18)));g.drawText("RACK DESIGNS",44,58,170,24,juce::Justification::centredLeft);g.setFont(caps.fontForCap(18));g.drawText("SEARCH",search.getX(),search.getY()-24,search.getWidth(),23,juce::Justification::centredLeft);
        if(state.detailOpen){look.drawGlass(g,preview.toFloat());if(const auto* row=chosen()){paintImage(g,*row,preview.reduced(7),2048);g.setColour(juce::Colour(0xffe0e2df));g.setFont(caps.fontForCap(24));g.drawText(row->metadata.name.toUpperCase(),content.getX()+8,preview.getBottom()+8,content.getWidth()-16,34,juce::Justification::centredLeft);}}
    }
};
LibraryUnit::LibraryUnit(InstrumentProcessor& p,const Model& m,StudioServices,Host h):browser(std::make_unique<Browser>(*this,p,m,std::move(h))){setSize(1547,740);}
LibraryUnit::~LibraryUnit()=default;
void LibraryUnit::selectRack(juce::Uuid id){browser->selectedRack=id;}
void LibraryUnit::refresh(){browser->refresh();}
void LibraryUnit::cancel(){if(browser->clientView&&browser->clientView->busy&&browser->clientView->job==browser->job)browser->runtime->client->cancel(browser->job);browser->runtime->client->cancelPlacement(browser->preparation);browser->message="Cancelled; live sound and saved racks preserved";browser->refreshStatus();}
void LibraryUnit::visibilityChanged(){
    if(!browser)return;
    if(!isVisible()){browser->openPending=false;browser->runtime->client->cancelPlacement(browser->preparation);browser->images.clear();browser->imageBytes=0;return;}
    browser->openPending=true;
    browser->refresh();browser->resolveSelection();
    if(isShowing()){if(browser->state.detailOpen)browser->back.grabKeyboardFocus();else browser->search.grabKeyboardFocus();}
}
void LibraryUnit::resized(){if(browser)browser->layout();}
void LibraryUnit::paint(juce::Graphics& g){browser->paint(g);}
bool LibraryUnit::keyPressed(const juce::KeyPress& k){if(k.getKeyCode()!=juce::KeyPress::escapeKey)return false;if(browser->state.detailOpen||browser->filterOpen)browser->goBack();else{cancel();if(onDone)onDone();}return true;}
std::optional<StackPlacement> LibraryUnit::placement() const{return std::nullopt;}
const RackLibraryBrowserState& LibraryUnit::browserState() const{return browser->state;}
std::size_t LibraryUnit::matchingRows() const{return browser->matches.size();}
std::uint64_t LibraryUnit::ownedImageBytes() const{return browser->imageBytes;}
}
