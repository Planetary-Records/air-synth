#pragma once
// Included after the existing VST3 host, state and audio helpers.
int renderFactorySounds(juce::StringArray args)
{
    args.remove(0);const auto selected=parseArguments(args);
    require(!selected.output.exists(),"Factory sound output must be fresh");
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types,selected.plugin.getFullPathName());
    require(types.size()==1&&isAirInstrument(*types[0]),"Factory sounds require the air VST3");
    require(selected.output.createDirectory().wasOk(),"Cannot create factory output");
    constexpr int rate=48000,frames=6*rate;
    std::vector<Event> events {{0,juce::MidiMessage::noteOn(1,48,juce::uint8(100))},
                              {rate,juce::MidiMessage::noteOff(1,48)}};
    for(int note:{48,52,55,60})events.push_back({2*rate,juce::MidiMessage::noteOn(1,note,juce::uint8(100))});
    for(int note:{48,52,55,60})events.push_back({3*rate,juce::MidiMessage::noteOff(1,note)});
    auto midi=events;for(auto& event:midi)event.sample/=50;
    writeMidiEvents(selected.output.getChildFile("Audition.mid"),midi);
    juce::var report(new juce::DynamicObject);auto* object=report.getDynamicObject();
    object->setProperty("complete",false);object->setProperty("success",false);
    object->setProperty("sample_rate",rate);object->setProperty("frames_per_sound",frames);
    object->setProperty("module_sha256",hash(selected.plugin.getChildFile("Contents/x86_64-win/AI Rack Synth.vst3")));
    juce::Array<juce::var> rows;
    const auto receipt=[&]{object->setProperty("sounds",juce::var(rows));
        const auto text=juce::JSON::toString(report,true);
        require(selected.output.getChildFile("factory-sounds.json").replaceWithText(text),"Factory receipt write failed");};
    receipt();require(airack::factorySounds().size()>=12,"Twelve sounds required");
    for(std::size_t i=0;i<airack::factorySounds().size();++i){
        const auto& info=airack::factorySounds()[i];std::cout<<"FACTORY_SOUND "<<info.name<<std::endl;
        airack::SoundDocument sound;require(airack::makeFactorySound(i,sound),"Invalid factory sound");
        juce::MemoryBlock bytes;require(airack::encodeSoundState(sound,bytes),"Factory encoding failed");
        const auto stem=juce::String(info.name).replaceCharacter(' ','-').toLowerCase();
        const auto file=selected.output.getChildFile(stem+".airack");writeBlock(file,bytes);
        juce::MemoryBlock saved;airack::SoundDocument decoded;
        require(file.loadFileAsData(saved)&&saved==bytes&&airack::decodeSoundState(saved.getData(),saved.getSize(),decoded)&&decoded==sound,"Saved factory readback failed");
        const auto initial=render(format,*types[0],rate,128,hostEnvelope(saved),sound,airack::currentSoundStateSize,false,-1,&events,frames);
        const auto recalled=render(format,*types[0],rate,512,initial.hostState,sound,airack::currentSoundStateSize,false,-1,&events,frames);
        const auto measures=measureWindow(initial.audio,{{0,frames}});
        const auto error=difference(initial.audio,recalled.audio);
        require(measures.rms>1.e-5&&measures.peak<.99&&measures.excursions==0,"Factory sound silent or clipped");
        require(error==0,"Factory recall/partition output differs");
        const auto wav=selected.output.getChildFile(stem+".wav");writeWav(wav,initial.audio,rate);
        juce::var row(new juce::DynamicObject);auto* item=row.getDynamicObject();
        item->setProperty("name",info.name);item->setProperty("description",info.description);
        item->setProperty("sound_file",file.getFileName());item->setProperty("sound_sha256",hash(file));
        item->setProperty("wav_file",wav.getFileName());item->setProperty("wav_sha256",hash(wav));
        item->setProperty("peak",measures.peak);item->setProperty("rms",measures.rms);item->setProperty("recall_partition_difference",error);
        item->setProperty("success",true);rows.add(row);receipt();
    }
    object->setProperty("complete",true);object->setProperty("success",true);receipt();
    std::cout<<"PASS: twelve factory sounds exported and rendered through the VST3\n";return 0;
}
