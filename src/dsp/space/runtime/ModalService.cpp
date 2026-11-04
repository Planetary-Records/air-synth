#include "ModalService.h"
#include "ModalConverter.h"
#include "SpaceReverbMailbox.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <process.h>
#endif
namespace airack::space::runtime {
std::unique_ptr<const ModalReadyBundle> prepareModalBundle(double rate,std::size_t modes,std::uint64_t key,const ModalSlotSettings& settings,const ModalSlotIdentities& ids,detail::PreparationControl control,unsigned factor) noexcept {
    if((factor!=1&&factor!=2)||!std::isfinite(rate)||rate*factor<8000||rate*factor>768000)return {};PreparedModalConverter converter;if(!prepareModalConverter(factor,converter))return {};
    if(!key)return {};for(unsigned i=0;i<8;++i)if(!ids[i]||!valid(settings[i]))return {};
    try {control.checkpoint();std::unique_ptr<ModalReadyBundle> b(new ModalReadyBundle);if(!prepareModalTailContext(rate,modes,b->context_,{converter.logicalSupport(),converter.logGain()}))return {};
        for(unsigned i=0;i<8;++i){control.checkpoint();auto& p=b->slots_[i];p.settings=settings[i];p.identity=ids[i];if(!prepareModalRoom(p.settings.decay,p.settings.size,rate,modes,p.room,control)||!prepareModalWet(p.settings,rate,p.wet)||!prepareModalFinal(p.settings,rate*factor,p.final))return {};control.checkpoint();if(!prepareModalRoomTail(b->context_,p.room,p.roomTail)||!prepareModalPostTail(b->context_,p.wet.post,p.postTail))return {};}
        control.checkpoint();b->key_=key;b->factor_=factor;return b;
    }catch(...){return {};}
}
namespace {
struct RoomKey {DecayCurve decay;double size=.5;bool operator==(const RoomKey&) const=default;};
struct DuckKey {double range=0,threshold=-24,attack=10,release=250;bool operator==(const DuckKey&) const=default;};
using PostKey=std::array<PostBand,6>;
RoomKey roomKey(const Settings& s) noexcept {return {s.decay,s.size};}DuckKey duckKey(const Settings& s) noexcept {return {s.duckRangeDb,s.duckThresholdDb,s.duckAttackMs,s.duckReleaseMs};}
struct Request {Settings settings;std::uint64_t identity=0,epoch=0,generation=0;};
struct RoomResult {Request request;bool success=false;PreparedModalRoom room;PreparedModalEarly early;PreparedModalTail tail;};
struct PostResult {Request request;bool success=false;PreparedModalPost post;PreparedModalTail tail;};
struct DuckResult {Request request;bool success=false;PreparedModalDuck duck;};
static_assert(std::is_trivially_copyable_v<RoomResult>&&std::is_trivially_copyable_v<PostResult>);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
constexpr std::size_t statusWords=6+8*11;
static_assert(sizeof(ModalServiceStatus)==statusWords*sizeof(std::uint64_t));
struct Published {
    std::atomic<std::uint64_t> sequence{0};std::array<std::atomic<std::uint64_t>,statusWords> words{};
    void write(const ModalServiceStatus& s) noexcept {sequence.fetch_add(1);const auto data=std::bit_cast<std::array<std::uint64_t,statusWords>>(s);for(std::size_t i=0;i<statusWords;++i)words[i].store(data[i]);sequence.fetch_add(1);}
    bool read(ModalServiceStatus& out) const noexcept {for(unsigned attempt=0;attempt<4;++attempt){const auto before=sequence.load();if(before&1)continue;std::array<std::uint64_t,statusWords> data;for(std::size_t i=0;i<statusWords;++i)data[i]=words[i].load();if(sequence.load()!=before)continue;const auto s=std::bit_cast<ModalServiceStatus>(data);if(!s.documentKey)return false;out=s;return true;}return false;}
};
struct Slot {
    std::array<ModalRoom,2> rooms;ModalWetOuter wet;ModalFinalOuter final;ModalLiveTail tail;
    std::array<PreparedModalEarly,2> early;std::array<PreparedModalTail,2> roomTail;PreparedModalTail postTail,pendingPostTail;
    Settings latest;RoomKey soundingRoom,requestedRoom,pendingRoom;PostKey soundingPost,requestedPost,pendingPost;DuckKey soundingDuck,requestedDuck;
    std::uint64_t identity=0,epoch=0,roomGeneration=1,postGeneration=1,duckGeneration=1,appliedRoom=1,appliedPost=1,appliedDuck=1,pendingGeneration=0,failures=0;
    const RoomResult* roomView=nullptr;const PostResult* postView=nullptr;const DuckResult* duckView=nullptr;
    unsigned active=0;std::size_t rendered=0,phase=0;bool force=false,done=false;
    ModalWetConverter converter;std::array<ModalRoom::Frame,512> logical{},extra{};std::array<double,512> alpha{};
};
}
struct ModalService::State {
    double rate=0,physicalRate=0;unsigned factor=1;std::size_t modes=0,duration=0,frames=0;std::uint64_t key=0,completed=0;unsigned mask=0,ticket=16,cursor=0,heavyCursor=0,lightCursor=0,admissions=0,maximumRooms=0;bool ready=false,open=false;
    ModalServiceOptions options;ModalTailContext context;PreparedModalConverter converterDescriptor;std::array<Slot,8> slots;
    std::array<detail::LatestMailbox<Request>,8> roomRequests,postRequests,duckRequests;
    std::array<detail::LatestMailbox<RoomResult>,8> roomResults;std::array<detail::LatestMailbox<PostResult>,8> postResults;std::array<detail::LatestMailbox<DuckResult>,8> duckResults;
    RoomResult roomScratch;PostResult postScratch;DuckResult duckScratch;
    std::array<std::atomic<std::uint64_t>,8> epochs{};std::atomic<bool> stopping{false};Published published;
    struct Worker {State* state=nullptr;unsigned index=0;};std::array<Worker,2> workerContexts;std::array<std::atomic<int>,2> startup{};
#if defined(_WIN32)
    std::array<HANDLE,2> threads{};
    static unsigned __stdcall entry(void* ptr) noexcept {auto& w=*static_cast<Worker*>(ptr);const bool priority=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL)!=0;w.state->startup[w.index].store(priority?1:-1,std::memory_order_release);if(priority)w.state->run(w.index);return 0;}
#else
    std::array<std::thread,2> threads;
#endif
    bool running=false;
    ~State(){stop();}
    void stop() noexcept {stopping.store(true,std::memory_order_release);
#if defined(_WIN32)
        for(auto& handle:threads)if(handle){WaitForSingleObject(handle,INFINITE);CloseHandle(handle);handle=nullptr;}
#else
        for(auto& t:threads)if(t.joinable())t.join();
#endif
        running=false;
    }
    bool start(){if(!options.threaded||running)return true;stopping.store(false,std::memory_order_release);for(auto& value:startup)value.store(0,std::memory_order_relaxed);
        for(unsigned i=0;i<2;++i){workerContexts[i]={this,i};
#if defined(_WIN32)
        unsigned id=0;threads[i]=reinterpret_cast<HANDLE>(_beginthreadex(nullptr,1024*1024,&entry,&workerContexts[i],STACK_SIZE_PARAM_IS_A_RESERVATION,&id));if(!threads[i]){stop();return false;}
#else
        try {threads[i]=std::thread([this,i]{startup[i].store(1,std::memory_order_release);run(i);});}catch(...){stop();return false;}
#endif
        while(startup[i].load(std::memory_order_acquire)==0)std::this_thread::yield();if(startup[i].load(std::memory_order_acquire)<0){stop();return false;}}
        running=true;return true;}
    void run(unsigned index) noexcept {while(!stopping.load(std::memory_order_acquire)){const bool work=index==0?pumpHeavy():pumpLight();if(!work)std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
    struct Control {State* state;ModalPreparationTag tag;bool first=true;static bool cancelled(void* p) noexcept {auto& c=*static_cast<Control*>(p);if(c.first){c.first=false;if(c.state->options.checkpoint)c.state->options.checkpoint(c.state->options.checkpointContext,c.tag,{&cancelled,&c});}return c.state->stopping.load(std::memory_order_acquire)||c.state->epochs[c.tag.slot].load(std::memory_order_acquire)!=c.tag.epoch;}};
    bool current(unsigned slot,const Request& r) const noexcept {return !stopping.load(std::memory_order_acquire)&&epochs[slot].load(std::memory_order_acquire)==r.epoch;}
    bool pumpHeavy() noexcept {for(unsigned n=0;n<8;++n){const unsigned i=(heavyCursor+n)%8;Request r;if(!roomRequests[i].take(r))continue;heavyCursor=(i+1)%8;if(!current(i,r))return true;auto& out=roomScratch;out.request=r;out.success=false;Control check{this,{i,ModalPreparationDomain::room,r.epoch,r.generation}};detail::PreparationControl control{&Control::cancelled,&check};
            try {out.success=prepareModalRoom(r.settings.decay,r.settings.size,rate,modes,out.room,control)&&prepareModalEarly(r.settings.size,rate,out.early);control.checkpoint();out.success=out.success&&prepareModalRoomTail(context,out.room,out.tail);control.checkpoint();}catch(...){out.success=false;}
            if(current(i,r))roomResults[i].publish(out);return true;}return false;}
    bool pumpLight() noexcept {for(unsigned n=0;n<16;++n){const unsigned d=(lightCursor+n)%16,i=d/2;Request r;const bool duck=d%2!=0;if(!(duck?duckRequests[i].take(r):postRequests[i].take(r)))continue;lightCursor=(d+1)%16;if(!current(i,r))return true;Control check{this,{i,duck?ModalPreparationDomain::duck:ModalPreparationDomain::post,r.epoch,r.generation}};detail::PreparationControl control{&Control::cancelled,&check};
            try {control.checkpoint();if(duck){duckScratch.request=r;duckScratch.success=prepareModalDuck(r.settings,physicalRate,duckScratch.duck);control.checkpoint();if(current(i,r))duckResults[i].publish(duckScratch);}else {postScratch.request=r;postScratch.success=prepareModalPost(r.settings,rate,postScratch.post)&&prepareModalPostTail(context,postScratch.post,postScratch.tail);control.checkpoint();if(current(i,r))postResults[i].publish(postScratch);}}catch(...){}return true;}return false;}
    void publish() noexcept {ModalServiceStatus s;s.documentKey=key;s.completedParents=completed;s.ticketSlot=ticket<16?ticket/2:8;s.ticketDomain=ticket<16?ticket%2:0;s.admissionsThisParent=admissions;s.maximumRoomsThisParent=maximumRooms;for(unsigned i=0;i<8;++i){const auto& p=slots[i];auto& d=s.slots[i];d.identity=p.identity;d.epoch=p.epoch;d.requestedRoom=p.roomGeneration;d.requestedPost=p.postGeneration;d.requestedDuck=p.duckGeneration;d.appliedRoom=p.appliedRoom;d.appliedPost=p.appliedPost;d.appliedDuck=p.appliedDuck;d.preparationFailures=p.failures;ModalTailReport tail;if(p.tail.report(tail))d.tailFrames=tail.frames>UINT64_MAX/factor?UINT64_MAX:tail.frames*factor;d.peakExponent=std::uint64_t(p.tail.peakExponent());}published.write(s);}
    void commit(){if(ticket>=16)return;auto& s=slots[ticket/2];if(!s.done)return;if(ticket%2==0){s.active=1-s.active;s.soundingRoom=s.pendingRoom;s.appliedRoom=s.pendingGeneration;s.wet.setEarlyGeometries(s.early[s.active],s.early[s.active]);}else {s.wet.commitPost();s.soundingPost=s.pendingPost;s.postTail=s.pendingPostTail;s.appliedPost=s.pendingGeneration;}cursor=(ticket+1)%16;ticket=16;}
    void reset(unsigned i) noexcept {auto& s=slots[i];if(ticket/2==i){commit();ticket=16;}s.rooms[0].reset();s.rooms[1].reset();s.wet.reset();s.final.reset();s.converter.reset();s.tail.reset(s.roomTail[s.active],s.postTail);++s.epoch;epochs[i].store(s.epoch,std::memory_order_release);s.force=true;s.roomView=nullptr;s.postView=nullptr;s.duckView=nullptr;s.phase=0;s.done=false;}
    void request(unsigned i,const Settings& target) noexcept {auto& s=slots[i];s.latest=target;if(!(mask&(1U<<i)))return;s.wet.setFastTargets({target.predelayMs,target.character,target.distance});s.final.setFastTargets(target.width,target.mix);
        const auto room=roomKey(target);const auto duck=duckKey(target);if(s.force||room!=s.requestedRoom){s.requestedRoom=room;roomRequests[i].publish({target,s.identity,s.epoch,++s.roomGeneration});}if(s.force||target.post!=s.requestedPost){s.requestedPost=target.post;postRequests[i].publish({target,s.identity,s.epoch,++s.postGeneration});}if(s.force||duck!=s.requestedDuck){s.requestedDuck=duck;duckRequests[i].publish({target,s.identity,s.epoch,++s.duckGeneration});}s.force=false;}
    template<class Result> bool validResult(const Slot& s,const Result* p,std::uint64_t requested,std::uint64_t applied) const noexcept {return p&&p->request.identity==s.identity&&p->request.epoch==s.epoch&&p->request.generation<=requested&&p->request.generation>applied;}
    void poll(unsigned i) noexcept {auto& s=slots[i];if(auto* p=roomResults[i].takeView())s.roomView=p;if(auto* p=postResults[i].takeView())s.postView=p;if(auto* p=duckResults[i].takeView())s.duckView=p;
        if(!validResult(s,s.roomView,s.roomGeneration,s.appliedRoom))s.roomView=nullptr;if(s.roomView&&!s.roomView->success){++s.failures;s.roomView=nullptr;}if(!validResult(s,s.postView,s.postGeneration,s.appliedPost))s.postView=nullptr;if(s.postView&&!s.postView->success){++s.failures;s.postView=nullptr;}
        if(!validResult(s,s.duckView,s.duckGeneration,s.appliedDuck))s.duckView=nullptr;if(s.duckView&&(mask&(1U<<i))){if(s.duckView->success){s.final.adoptDuck(s.duckView->duck);s.soundingDuck=duckKey(s.duckView->request.settings);s.appliedDuck=s.duckView->request.generation;}else ++s.failures;s.duckView=nullptr;}}
    void admit() noexcept {if(ticket<16)return;const auto first=cursor;for(unsigned n=0;n<16;++n){const auto d=(first+n)%16,i=d/2;if(!(mask&(1U<<i)))continue;auto& s=slots[i];if(d%2==0){const auto* p=s.roomView;if(!p)continue;const auto next=roomKey(p->request.settings);if(next==s.soundingRoom){s.appliedRoom=p->request.generation;s.roomView=nullptr;cursor=(d+1)%16;continue;}
                if(!(options.preparedCopies?s.rooms[1-s.active].adoptPreparedCopied(p->room,s.rooms[s.active]):s.rooms[1-s.active].adoptCopied(p->room,s.rooms[s.active]))){++s.failures;s.roomView=nullptr;continue;}s.early[1-s.active]=p->early;s.roomTail[1-s.active]=p->tail;s.wet.setEarlyGeometries(s.early[s.active],p->early);s.tail.admitRoom(p->tail);s.pendingRoom=next;s.pendingGeneration=p->request.generation;s.roomView=nullptr;
            }else {const auto* p=s.postView;if(!p)continue;if(p->request.settings.post==s.soundingPost){s.appliedPost=p->request.generation;s.postView=nullptr;cursor=(d+1)%16;continue;}if(!s.wet.adoptPost(p->post)){++s.failures;s.postView=nullptr;continue;}s.tail.admitPost(p->tail);s.pendingPost=p->request.settings.post;s.pendingPostTail=p->tail;s.pendingGeneration=p->request.generation;s.postView=nullptr;}
            ticket=d;s.phase=0;s.done=false;admissions=1;return;}}
    bool renderWet(unsigned i,std::span<ModalRoom::Frame> frames) noexcept {
        if(frames.empty())return true;auto& slot=slots[i];const bool selected=ticket<16&&ticket/2==i,roomFade=selected&&ticket%2==0,postFade=selected&&!roomFade;
        auto alpha=std::span(slot.alpha).first(frames.size());if(selected)for(auto& a:alpha){if(slot.phase==0)a=0;else if(slot.phase>=duration)a=1;else a=double(slot.phase)/duration;if(slot.phase>=duration)slot.done=true;else ++slot.phase;}
        if(!slot.wet.excite(frames))return false;if(roomFade)std::copy(frames.begin(),frames.end(),slot.extra.begin());if(!slot.rooms[slot.active].processBlock(frames,slot.wet.characters()))return false;
        if(roomFade){auto extra=std::span(slot.extra).first(frames.size());if(!slot.rooms[1-slot.active].processBlock(extra,slot.wet.characters())||!slot.wet.mixRooms(frames,extra,alpha))return false;}else if(!slot.wet.mixRooms(frames))return false;
        return slot.wet.post(frames,postFade?std::span<const double>(alpha):std::span<const double>{});
    }
};
ModalService::ModalService()=default;ModalService::~ModalService()=default;
bool ModalService::prepareStorage(double rate,std::size_t modes,ModalServiceOptions options){
    if((options.factor!=1&&options.factor!=2)||!std::isfinite(rate)||rate*options.factor<8000||rate*options.factor>768000)return false;PreparedModalConverter converter;ModalTailContext context;
    if(!prepareModalConverter(options.factor,converter)||!prepareModalTailContext(rate,modes,context,{converter.logicalSupport(),converter.logGain()}))return false;
    try {auto s=std::make_unique<State>();s->rate=rate;s->factor=options.factor;s->physicalRate=rate*options.factor;s->converterDescriptor=converter;s->modes=modes;s->duration=std::size_t(std::ceil(.005*rate));s->options=options;s->context=context;
        for(auto& slot:s->slots){for(auto& room:slot.rooms)if(!room.prepareStorage(rate,ModalBackend::automatic,options.blockedScatter,options.pairedReduction))return false;if(!slot.wet.prepareStorage(rate)||!slot.final.prepareStorage(s->physicalRate)||!slot.converter.configure(converter,options.converterBackend))return false;}
        if(!s->start())return false;state_=std::move(s);return true;}catch(...){return false;}
}
bool ModalService::installBundle(const ModalReadyBundle& bundle,std::uint64_t expected,unsigned mask) noexcept {if(!state_||state_->open||!expected||mask==0||mask>255||bundle.key_!=expected||bundle.rate()!=state_->rate||bundle.modes()!=state_->modes||bundle.factor_!=state_->factor)return false;auto& s=*state_;
    if(!s.ready&&mask!=255)return false;for(unsigned i=0;i<8;++i)if(!(mask&(1U<<i))&&bundle.slots_[i].identity!=s.slots[i].identity)return false;
    if(mask==255){s.ticket=16;s.mask=0;s.cursor=0;s.admissions=s.maximumRooms=0;s.completed=0;}else{if(s.ticket<16&&(mask&(1U<<(s.ticket/2))))s.ticket=16;s.mask&=~mask;}
    for(unsigned i=0;i<8;++i){if(!(mask&(1U<<i)))continue;auto& slot=s.slots[i];const auto& p=bundle.slots_[i];++slot.epoch;s.epochs[i].store(slot.epoch,std::memory_order_release);if(!slot.rooms[0].configureImpl(p.room,s.options.preparedCopies)||!slot.wet.configure(p.wet)||!slot.final.configure(p.final)||!slot.converter.configure(s.converterDescriptor,s.options.converterBackend)||!slot.tail.configure(bundle.context_,p.roomTail,p.postTail))return false;
        // A spare room is fully overwritten by the next copied admission before use.
        if(!s.options.preparedCopies)slot.rooms[1].reset();slot.active=0;slot.latest=p.settings;slot.identity=p.identity;slot.soundingRoom=slot.requestedRoom=roomKey(p.settings);slot.soundingPost=slot.requestedPost=p.settings.post;slot.soundingDuck=slot.requestedDuck=duckKey(p.settings);slot.roomGeneration=slot.postGeneration=slot.duckGeneration=slot.appliedRoom=slot.appliedPost=slot.appliedDuck=1;slot.early={p.wet.early,p.wet.early};slot.roomTail={p.roomTail,p.roomTail};slot.postTail=p.postTail;slot.force=slot.done=false;slot.phase=slot.rendered=0;slot.failures=0;slot.roomView=nullptr;slot.postView=nullptr;slot.duckView=nullptr;}
    s.key=bundle.key_;s.ready=true;s.publish();return true;
}
bool ModalService::beginParent(const ModalSlotSettings& settings,unsigned mask,const ModalSlotIdentities& ids,std::size_t frames,bool allowNewAdmission) noexcept {if(!state_||!state_->ready||state_->open||mask>255||frames==0||frames>1024*state_->factor)return false;auto& s=*state_;for(unsigned i=0;i<8;++i)if(!valid(settings[i])||ids[i]!=s.slots[i].identity)return false;
    const auto old=s.mask;s.mask=mask;s.frames=frames;s.admissions=0;for(unsigned i=0;i<8;++i){if((old&(1U<<i))&&!(mask&(1U<<i)))s.reset(i);s.slots[i].rendered=0;s.request(i,settings[i]);s.poll(i);}if(allowNewAdmission)s.admit();s.maximumRooms=std::popcount(mask)+unsigned(s.ticket<16&&s.ticket%2==0);s.open=true;return true;
}
bool ModalService::renderSlot(std::size_t i,std::span<Frame> frames) noexcept {
    if(!state_||!state_->open||i>=8||!(state_->mask&(1U<<i))||frames.size()>512)return false;auto& s=*state_;auto& slot=s.slots[i];if(frames.size()>s.frames-slot.rendered)return false;
    double peak=0;for(auto f:frames)for(double v:f){if(!std::isfinite(v))return false;peak=std::max(peak,std::abs(v));}if(!slot.tail.observePeak(peak)||!slot.final.capture(frames))return false;
    if(s.factor==1){if(!s.renderWet(unsigned(i),frames))return false;}else {std::size_t produced=0;if(!slot.converter.down(frames,slot.logical,produced))return false;auto logical=std::span(slot.logical).first(produced);if(!s.renderWet(unsigned(i),logical)||!slot.converter.up(logical,frames))return false;}
    if(!slot.final.finish(frames))return false;slot.rendered+=frames.size();return true;
}
bool ModalService::endParent() noexcept {if(!state_||!state_->open)return false;auto& s=*state_;for(unsigned i=0;i<8;++i)if((s.mask&(1U<<i))&&s.slots[i].rendered!=s.frames)return false;s.commit();s.open=false;++s.completed;s.publish();return true;}
bool ModalService::resetSlot(std::size_t i) noexcept {if(!state_||!state_->ready||state_->open||i>=8)return false;state_->reset(unsigned(i));return true;}
bool ModalService::startWorkers() noexcept {if(!state_||state_->open||state_->mask!=0)return false;auto& s=*state_;s.options.threaded=true;try{if(s.start())return true;}catch(...){}s.stop();s.options.threaded=false;return false;}
bool ModalService::stopWorkers() noexcept {if(!state_||state_->open||state_->mask!=0)return false;state_->stop();state_->options.threaded=false;return true;}
bool ModalService::workersRunning() const noexcept {return state_&&state_->running;}
bool ModalService::makeInactive() noexcept {if(!state_||state_->open)return false;auto& s=*state_;if(s.ready){for(unsigned i=0;i<8;++i)if(s.mask&(1U<<i))s.reset(i);s.mask=0;s.publish();}return true;}
unsigned ModalService::admissionsThisParent() const noexcept {return state_?state_->admissions:0;}
bool ModalService::pumpHeavy() noexcept {return state_&&!state_->options.threaded&&state_->pumpHeavy();}bool ModalService::pumpLight() noexcept {return state_&&!state_->options.threaded&&state_->pumpLight();}
bool ModalService::composeTail(std::span<const std::size_t> indices,unsigned sources,ModalTailReport& out) const noexcept {
    if(!state_||!state_->ready||state_->open||indices.empty()||indices.size()>8)return false;std::array<const ModalLiveTail*,8> nodes{};for(std::size_t n=0;n<indices.size();++n){if(indices[n]>=8)return false;nodes[n]=&state_->slots[indices[n]].tail;}return composeModalLiveTail(std::span(nodes).first(indices.size()),sources,out);
}
bool ModalService::snapshot(ModalServiceStatus& out) const noexcept {return state_&&state_->published.read(out);}
bool ModalService::usesPairedReduction() const noexcept {if(!state_||!state_->ready)return false;for(const auto& slot:state_->slots)if(!slot.rooms[slot.active].usesPairedReduction())return false;if(state_->ticket<16&&state_->ticket%2==0){const auto& slot=state_->slots[state_->ticket/2];if(!slot.rooms[1-slot.active].usesPairedReduction())return false;}return true;}
bool ModalService::usesPreparedCopies() const noexcept {return state_&&state_->options.preparedCopies;}
bool ModalService::usesBlockedScatter() const noexcept {if(!state_)return false;for(const auto& slot:state_->slots)for(const auto& room:slot.rooms)if(!room.usesBlockedScatter())return false;return true;}
std::optional<ModalConverterBackend> ModalService::converterBackend() const noexcept {if(!state_||!state_->ready)return {};const auto backend=state_->slots[0].converter.selectedBackend();if(backend==ModalConverterBackend::automatic)return {};for(const auto& slot:state_->slots)if(slot.converter.selectedBackend()!=backend)return {};return backend;}
std::size_t ModalService::modeCount() const noexcept {
    if(!state_||!state_->ready)return 0;const auto& s=*state_;
    for(const auto& slot:s.slots)if(slot.rooms[slot.active].prepared().modal.count!=s.modes)return 0;
    if(s.ticket<16&&s.ticket%2==0){const auto& slot=s.slots[s.ticket/2];if(slot.rooms[1-slot.active].prepared().modal.count!=s.modes)return 0;}
    return s.modes;
}
std::size_t ModalService::storageBytes() const noexcept {if(!state_)return sizeof(*this);std::size_t bytes=sizeof(*this)+sizeof(State);for(const auto& s:state_->slots){for(const auto& r:s.rooms)bytes+=r.storageBytes()-sizeof(r);bytes+=s.wet.storageBytes()-sizeof(s.wet)+s.final.storageBytes()-sizeof(s.final);}return bytes+2*(1024*1024+64*1024)+2*1024*1024;}
}
