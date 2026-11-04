#pragma once
// Included after the renderer's existing host, state and matrix helpers.
int alphaSoakSeconds(const juce::String& text)
{
    const auto raw=text.toStdString();int seconds=0;const auto parsed=std::from_chars(raw.data(),raw.data()+raw.size(),seconds);
    require(parsed.ec==std::errc{}&&parsed.ptr==raw.data()+raw.size()&&seconds>=1&&seconds<=3600,"Soak seconds must be an integer from1 to3600");return seconds;
}
int alphaSmokeSelfTest()
{
    require(alphaSoakSeconds("1")==1&&alphaSoakSeconds("3600")==3600,"Soak duration boundaries rejected");
    for(const char* value:{"0","3601","-1","1.0","1x",""}){bool rejected=false;try{alphaSoakSeconds(value);}catch(const std::exception&){rejected=true;}require(rejected,"Invalid soak duration accepted");}
    std::cout<<"Alpha smoke duration: 8 checks passed\n";return 0;
}
int renderAlphaSmoke(juce::StringArray args)
{
    require(args.size()==7&&args[0]=="--alpha-smoke"&&args[1]=="--soak-seconds","Usage: --alpha-smoke --soak-seconds 1..3600 --plugin ABS_BUNDLE --output ABS_FRESH_WORKSPACE_DIRECTORY");
    const int seconds=alphaSoakSeconds(args[2]);args.removeRange(0,3);const auto selected=parseArguments(args);
    require(SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS)!=0,"Smoke process priority setup failed");
    using Clock=std::chrono::steady_clock;const auto began=Clock::now();
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> types;const auto scan=Clock::now();format.findAllTypesForFile(types,selected.plugin.getFullPathName());
    const auto scanMs=std::chrono::duration<double,std::milli>(Clock::now()-scan).count();require(types.size()==1&&isAirInstrument(*types[0]),"Alpha smoke requires the air VST3");
    require(renderFixtures(selected)==0,"Existing audio/recall matrix failed");
    auto document=std::make_unique<airack::SoundDocument>();document->values=fixtureValues(false);
    document->layout.synthUse.fill(airack::SlotUse::present);document->layout.synthOrder={0,1,2,3};document->layout.synthCount=4;
    juce::MemoryBlock owned;require(airack::encodeSoundState(*document,owned),"Smoke sound encoding failed");const auto state=hostEnvelope(owned);
    auto create=[&](int rate,int block,bool offline){juce::String error;auto instance=format.createInstanceFromDescription(*types[0],rate,block,error);
        require(instance!=nullptr&&instance->getTotalNumOutputChannels()==2&&instance->acceptsMidi(),"Smoke instance creation failed");
        instance->setNonRealtime(offline);instance->setStateInformation(state.getData(),int(state.getSize()));instance->prepareToPlay(rate,block);return instance;};
    auto renderOne=[&](int rate,int block,bool offline){auto instance=create(rate,block,offline);juce::AudioBuffer<float> audio(2,rate);audio.clear();juce::MidiBuffer midi;
        for(int offset=0;offset<rate;offset+=block){const int count=std::min(block,rate-offset);juce::AudioBuffer<float> buffer(2,count);buffer.clear();midi.clear();
            if(offset==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(96)),0);
            if(rate/2>=offset&&rate/2<offset+count)midi.addEvent(juce::MidiMessage::noteOff(1,60),rate/2-offset);
            instance->processBlock(buffer,midi);for(int ch=0;ch<2;++ch)audio.copyFrom(ch,offset,buffer,ch,0,count);}
        juce::MemoryBlock recalled;instance->getStateInformation(recalled);verifyReturnedState(recalled,*document,airack::currentSoundStateSize);instance->releaseResources();return audio;};
    juce::Array<juce::var> rows;
    for(int rate:{44100,48000,96000}){const auto reference=renderOne(rate,64,true);
        for(int block:{64,128,256,512,1024}){const auto offline=renderOne(rate,block,true),realtime=renderOne(rate,block,false);
            const auto partition=difference(reference,offline),mode=difference(offline,realtime);const auto metrics=measure(realtime,rate*3/4);
            require(partition<=1e-6&&mode<=1e-6&&metrics.peak>1e-6&&metrics.peak<1,"Smoke rate/buffer/realtime mismatch or invalid audio");
            auto row=jsonObject();property(row,"sample_rate",rate);property(row,"buffer",block);property(row,"partition_difference",partition);property(row,"offline_realtime_difference",mode);rows.add(row);}}
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> instances;for(int i=0;i<10;++i)instances.push_back(create(48000,256,false));
    const auto memory=[](){PROCESS_MEMORY_COUNTERS_EX value{};require(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value),sizeof(value))!=0,"Process memory observation failed");return juce::int64(value.PrivateUsage);};
    const auto privateBefore=memory();const auto soakStart=Clock::now();auto checkpoint=soakStart;std::uint64_t blocks=0,late=0;
    std::array<double,10> peaks{},windowPeaks{};juce::AudioBuffer<float> buffer(2,256);juce::MidiBuffer midi;
    while(std::chrono::duration<double>(Clock::now()-soakStart).count()<seconds||blocks*256<std::uint64_t(seconds)*48000){
        const auto frame=blocks*256;const auto phase=frame%24000;
        for(std::size_t i=0;i<instances.size();++i){buffer.clear();midi.clear();
            if(phase<256)midi.addEvent(juce::MidiMessage::noteOn(1,48+int(i),juce::uint8(96)),0);
            if(phase>=12000&&phase<12256)midi.addEvent(juce::MidiMessage::noteOff(1,48+int(i)),0);
            instances[i]->processBlock(buffer,midi);
            for(int ch=0;ch<2;++ch)for(int n=0;n<256;++n){const double sample=std::abs(double(buffer.getSample(ch,n)));require(std::isfinite(sample)&&sample<1,"Soak output is nonfinite or clipped");peaks[i]=std::max(peaks[i],sample);windowPeaks[i]=std::max(windowPeaks[i],sample);}}
        ++blocks;const auto now=Clock::now();
        if(now-checkpoint>=std::chrono::seconds(60)){for(auto peak:windowPeaks)require(peak>1e-8,"A soak instance stopped producing audio");windowPeaks.fill(0);checkpoint=now;
            auto progress=jsonObject();property(progress,"elapsed_seconds",std::chrono::duration<double>(now-soakStart).count());property(progress,"blocks_per_instance",juce::int64(blocks));property(progress,"private_bytes",memory());
            require(selected.output.getChildFile("alpha-smoke-progress.json").replaceWithText(juce::JSON::toString(progress)),"Soak progress write failed");}
        const auto due=soakStart+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(blocks)*256/48000));if(now>due)++late;else std::this_thread::sleep_until(due);
        personalityPump();
    }
    const double elapsed=std::chrono::duration<double>(Clock::now()-soakStart).count();
    for(std::size_t i=0;i<instances.size();++i){require(peaks[i]>1e-6,"Silent soak instance");juce::MemoryBlock recalled;instances[i]->getStateInformation(recalled);verifyReturnedState(recalled,*document,airack::currentSoundStateSize);instances[i]->releaseResources();}
    const auto privateAfter=memory();instances.clear();
    auto report=jsonObject();property(report,"success",true);property(report,"full_hour_soak",seconds==3600&&elapsed>=3600);property(report,"requested_soak_seconds",seconds);property(report,"soak_elapsed_seconds",elapsed);
    property(report,"simultaneous_instances",10);property(report,"blocks_per_instance",juce::int64(blocks));property(report,"scan_ms",scanMs);property(report,"rate_buffer_rows",rows);property(report,"private_bytes_before",privateBefore);property(report,"private_bytes_after",privateAfter);
    property(report,"rendered_frames_per_instance",juce::int64(blocks*256));property(report,"soak_sample_rate",48000);
    property(report,"cadence_late_observations",juce::int64(late));property(report,"qualification","Functional paced host only; no DAW deadline, listening or memory-growth budget acceptance. Short runs never qualify the required full-hour soak.");
    property(report,"total_seconds",std::chrono::duration<double>(Clock::now()-began).count());const auto text=juce::JSON::toString(report,true);writeBlock(selected.output.getChildFile("alpha-smoke.json"),juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8()));
    std::cout<<"ALPHA_SMOKE "<<(seconds==3600?"FULL_HOUR":"SHORT_DIAGNOSTIC")<<" seconds="<<elapsed<<" instances=10\n";return 0;
}
