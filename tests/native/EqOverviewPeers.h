#pragma once
#include "CompanionRuntimeServer.h"
#include "EqOverview.h"
#include "InstrumentProcessor.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <thread>
#include <stdexcept>
#include <iostream>

namespace airack {
struct InstrumentGroupTestAccess {
    static void pauseAutomaticPoll(InstrumentProcessor& p){p.stopTimer();}
};
}

// Four real admitted native clients and FFT publishers, on one private pipe.
// This adds spectrum analysis/IPC load, not four extra synth audio workloads.
struct EqOverviewPeers
{
    juce::File root;
    juce::String suffix=juce::Uuid().toString().removeCharacters("-");
    airack::GroupUuid group{juce::Uuid()};
    std::atomic_bool stop=false;
    std::atomic_uint providerJobs=0;
    std::atomic<int> serverStatus{-1};
    std::atomic_bool serverEntered=false;
    std::array<double,5> requestMs{};
    airack::companion::RuntimeServer server;
    std::thread io;
    std::array<std::unique_ptr<airack::InstrumentProcessor>,4> senders;
    std::array<std::shared_ptr<airack::studio::FxAnalysisHub>,4> hubs;
    std::array<std::optional<airack::studio::FxAnalysisHub::Lease>,4> leases;
    std::array<std::uint64_t,4> owners{},frames{};
    std::array<airack::GroupUuid,4> members{};
    double lastPublish=0;
    unsigned published=0,refused=0,pumpCount=0;
    // A controlled UI stall regression; ordinary fixtures use the real loop.
    int manualPollIntervalMs=0;
    explicit EqOverviewPeers(juce::File privateRoot):root(std::move(privateRoot)),
        server([this](const auto&){++providerJobs;return airack::companion::Frame{};},[](const auto&){})
    {
        if(!root.createDirectory().wasOk())throw std::runtime_error("peer fixture root");
        for(std::size_t i=0;i<4;++i){
            auto& p=senders[i];p=std::make_unique<airack::InstrumentProcessor>(options());
            auto d=std::make_unique<airack::SoundDocument>();d->layout.effects[7]={airack::SlotUse::present,airack::EffectKind::dynamicEq,airack::masterChain};d->layout.effectOrder[airack::masterChain][0]=7;d->layout.effectCount[airack::masterChain]=1;
            juce::MemoryBlock state;if(!airack::encodeSoundState(*d,state)||!p->restoreSoundState(state.getData(),int(state.getSize())))throw std::runtime_error("peer fixture sound");
            if(!p->setGroupIntent(intent("Source "+juce::String(int(i+1)))))throw std::runtime_error("peer fixture intent");
            members[i]=p->groupLinkView().intent->member;hubs[i]=std::make_shared<airack::studio::FxAnalysisHub>(p->fxMonitorHandle());
            leases[i]=hubs[i]->acquire(7);if(!leases[i])throw std::runtime_error("peer fixture capture lease");
        }
        io=std::thread([this]{serverEntered=true;const auto r=server.run(stop,suffix);serverStatus.store(int(r.status));if(r.workerRetained)std::terminate();});
    }
    ~EqOverviewPeers(){for(std::size_t i=0;i<4;++i){if(hubs[i])hubs[i]->retire();if(senders[i])senders[i]->closeAiSession();}senders={};stop=true;if(io.joinable())io.join();}
    airack::companion::ClientOptions options() const {return {{juce::File{},suffix,root},false};}
    airack::GroupIntent intent(const juce::String& role) const {
        airack::GroupIntent result;result.group=group;result.member=juce::Uuid();result.bpm=120;
        if(!airack::setGroupText("EQ overview fixture",result.name)||!airack::setGroupText(role.toStdString(),result.role)||!airack::setGroupText("A",result.key)||!airack::setGroupText("minor",result.scale))throw std::runtime_error("peer fixture metadata");return result;
    }
    template<class Predicate> bool wait(airack::InstrumentProcessor& receiver,Predicate ready){
        if(manualPollIntervalMs){airack::InstrumentGroupTestAccess::pauseAutomaticPoll(receiver);for(auto& p:senders)airack::InstrumentGroupTestAccess::pauseAutomaticPoll(*p);}
        const auto begin=juce::Time::getMillisecondCounterHiRes(),end=begin+5000;pumpCount=0;published=refused=0;
        const auto report=[&](bool success){std::cout<<"EQ_PEER_WAIT ready="<<success<<" elapsed_ms="<<juce::Time::getMillisecondCounterHiRes()-begin<<" manual_poll_ms="<<manualPollIntervalMs<<" pumps="<<pumpCount<<" published="<<published<<" refused="<<refused<<'\n';return success;};
        do{pump(receiver);if(ready())return report(true);if(manualPollIntervalMs)std::this_thread::sleep_for(std::chrono::milliseconds(manualPollIntervalMs));else juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}while(juce::Time::getMillisecondCounterHiRes()<end);failed(receiver,"wait_timeout");return report(false);
    }
    bool failed(airack::InstrumentProcessor& receiver,const char* phase){
        std::cout<<"EQ_PEER_ATTACH phase="<<phase<<" server_entered="<<serverEntered.load()<<" server_status="<<serverStatus.load()<<" provider_jobs="<<providerJobs.load();
        for(const auto ms:requestMs)std::cout<<" request_ms="<<ms;std::cout<<'\n';
        const auto show=[](const char* role,const airack::InstrumentProcessor& p){const auto view=p.groupWorkflowView();
            std::cout<<"EQ_PEER_CLIENT role="<<role<<" link="<<int(view.link.phase)<<" connection="<<int(view.connection.phase)
                     <<" status="<<int(view.connection.status)<<" error="<<view.connection.errorCode<<" message="<<view.message<<'\n';};
        show("receiver",receiver);for(const auto& p:senders)show("sender",*p);return false;
    }
    bool attach(airack::InstrumentProcessor& receiver){
        auto started=juce::Time::getMillisecondCounterHiRes();
        if(!receiver.setGroupIntent(intent("Reference"))||!receiver.requestGroupConnection())return failed(receiver,"receiver_request");
        requestMs[0]=juce::Time::getMillisecondCounterHiRes()-started;
        for(std::size_t i=0;i<senders.size();++i){started=juce::Time::getMillisecondCounterHiRes();if(!senders[i]->requestGroupConnection())return failed(receiver,"sender_request");requestMs[i+1]=juce::Time::getMillisecondCounterHiRes()-started;}
        if(!wait(receiver,[&]{return receiver.groupLinkView().phase==airack::GroupLinkPhase::admitted&&std::all_of(senders.begin(),senders.end(),[](const auto& p){return p->groupLinkView().phase==airack::GroupLinkPhase::admitted;});}))return failed(receiver,"all_admitted");
        for(std::size_t i=0;i<4;++i){owners[i]=senders[i]->openEqSpectrumView();if(!owners[i]||!senders[i]->configureEqSpectrumView(owners[i],true,7,{}))return failed(receiver,"spectrum_configure");}
        if(!receiver.browseGroups())return failed(receiver,"browse_request");
        return wait(receiver,[&]{return receiver.groupWorkflowView().members.size()==5;})||failed(receiver,"five_members");
    }
    void pump(airack::InstrumentProcessor& receiver){
        ++pumpCount;
        receiver.pollAi();for(auto& p:senders)p->pollAi();
        const auto now=juce::Time::getMillisecondCounterHiRes();if(now-lastPublish<100)return;lastPublish=now;
        for(std::size_t i=0;i<4;++i)if(owners[i]){
            auto monitor=senders[i]->fxMonitorHandle();airack::FxMonitor::ActiveSlots active{};active[7]=true;if(!monitor->beginBlock(192000,active))continue;
            for(int n=0;n<airack::studio::FxSpectrum::fftSize;++n,++frames[i]){const double t=double(frames[i])/192000;const double sample=.008*(std::sin(2*juce::MathConstants<double>::pi*(220.+i*70)*t)+std::sin(2*juce::MathConstants<double>::pi*(1600.+i*900)*t));monitor->capture(7,{sample,sample},{sample,sample});}
            hubs[i]->update();
            if(const auto* r=hubs[i]->result(*leases[i]);r&&r->valid){const auto db=airack::studio::sampleEqSpectrum(*r,true);std::array<std::int16_t,128> levels;for(std::size_t n=0;n<128;++n)levels[n]=std::int16_t(std::lround(db[n]*100));if(senders[i]->captureEqSpectrum(owners[i],7,r->generation,r->windows,r->sampleRate,levels))++published;else ++refused;}
        }
    }
};
