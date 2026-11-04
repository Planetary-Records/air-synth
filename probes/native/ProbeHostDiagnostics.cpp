#include "ProbeHostDiagnostics.h"
#include "ProbeProcessor.h"
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstchannelcontextinfo.h>
namespace probe
{
namespace
{
using namespace Steinberg;
juce::var object() { return new juce::DynamicObject; }
void put(juce::var& v, const juce::Identifier& name, juce::var value) { v.getDynamicObject()->setProperty(name, std::move(value)); }
template <size_t N>
juce::var stringResult(const std::array<Vst::TChar, N>& buffer, tresult result, size_t cap)
{
    auto value = object();
    put(value, "result", static_cast<int>(result));
    put(value, "status", result == kResultOk ? "supplied" : "query_failed");
    if (result == kResultOk)
    {
        auto count = static_cast<size_t>(std::find(buffer.begin(), buffer.end(), 0) - buffer.begin());
        auto kept = std::min(count, cap);
        // Avoid exporting half a UTF-16 surrogate pair at the observation boundary.
        if (kept > 0 && buffer[kept - 1] >= 0xd800 && buffer[kept - 1] <= 0xdbff) --kept;
        std::array<juce::CharPointer_UTF16::CharType, 513> text {};
        std::copy_n(buffer.begin(), kept, text.begin());
        put(value, "value", juce::String(juce::CharPointer_UTF16(text.data())));
        put(value, "truncated", count > kept);
        put(value, "capacity_reached", count >= N - 1);
    }
    return value;
}
juce::var readString(Vst::IAttributeList& list, const char* key)
{
    std::array<Vst::TChar, 514> buffer;
    buffer.fill(0xffff);
    return stringResult(buffer, list.getString(key, buffer.data(), static_cast<uint32>(sizeof(buffer))), 512);
}
juce::var readInt(Vst::IAttributeList& list, const char* key)
{
    int64 number = 0;
    const auto result = list.getInt(key, number);
    auto value = object();
    put(value, "result", static_cast<int>(result));
    put(value, "status", result == kResultOk ? "supplied" : "query_failed");
    if (result == kResultOk) put(value, "value", static_cast<juce::int64>(number));
    return value;
}
}
StateOriginObservation observeState(Steinberg::IBStream* stream, bool saving)
{
    auto value = object();
    put(value, "direction", saving ? "save" : "load");
    put(value, "interface_status", "unavailable");
    put(value, "list_status", "not_queried");
    put(value, "recognized", "unknown");
    if (stream == nullptr) { put(value, "interface_status", "null_stream"); return { value }; }
    void* queried = nullptr;
    const auto result = stream->queryInterface(Vst::IStreamAttributes_iid, &queried);
    put(value, "interface_result", static_cast<int>(result));
    if (result != kResultOk || queried == nullptr) return { value };
    auto* attributes = static_cast<Vst::IStreamAttributes*>(queried);
    // queryInterface owns this reference; getAttributes returns a borrowed list.
    const auto release = juce::ScopeGuard([&] { attributes->release(); });
    put(value, "interface_status", "available");
    std::array<Vst::TChar, 128> name;
    name.fill(0xffff);
    const auto nameResult = attributes->getFileName(name.data());
    put(value, "filename", stringResult(name, nameResult, 127));
    if (auto* list = attributes->getAttributes())
    {
        put(value, "list_status", "available");
        // Official vstpresetkeys.h definitions; this header is omitted from JUCE's SDK subset.
        put(value, "state_type", readString(*list, "StateType"));
        put(value, "path", readString(*list, "FilePathString"));
        const auto raw = value["state_type"]["value"].toString();
        if (!(bool) value["state_type"]["truncated"] && (raw == "Project" || raw == "Default" || raw == "TrackPreset"))
            put(value, "recognized", raw);
    }
    else put(value, "list_status", "null");
    return { value };
}
ChannelObservation observeChannel(Steinberg::Vst::IAttributeList* list)
{
    auto value = object();
    put(value, "list_status", list == nullptr ? "null" : "available");
    if (list != nullptr)
    {
        namespace C = Vst::ChannelContext;
        put(value, "name", readString(*list, C::kChannelNameKey));
        put(value, "uid", readString(*list, C::kChannelUIDKey));
        put(value, "namespace", readString(*list, C::kChannelIndexNamespaceKey));
        put(value, "colour", readInt(*list, C::kChannelColorKey));
        put(value, "runtime_id", readInt(*list, C::kChannelRuntimeIDKey));
        put(value, "index", readInt(*list, C::kChannelIndexKey));
        put(value, "namespace_order", readInt(*list, C::kChannelIndexNamespaceOrderKey));
        put(value, "location", readInt(*list, C::kChannelPluginLocationKey));
    }
    return { value };
}
namespace
{
struct Sink
{
    juce::CriticalSection lock;
    std::array<juce::var, 256> records;
    juce::int64 sequence = 0, dropped = 0;
    juce::String start;
    int pid = 0;
    Sink()
    {
       #if JUCE_WINDOWS
        pid = static_cast<int>(GetCurrentProcessId());
        FILETIME created {}, exited {}, kernel {}, user {};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            start = juce::String::toHexString((static_cast<juce::int64>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
        else start = "unavailable";
       #else
        start = "unavailable";
       #endif
    }
};
Sink& sink() { static Sink value; return value; }
}
void recordState(juce::AudioProcessor& processor, Steinberg::IBStream* stream, bool saving)
{
    if (auto* p = dynamic_cast<ProbeProcessor*>(&processor))
        ProbeHostDiagnostics::record(p->runtimeIdentity(), "stream", observeState(stream, saving).fields);
}
void recordChannel(juce::AudioProcessor& processor, Steinberg::Vst::IAttributeList* list)
{
    if (auto* p = dynamic_cast<ProbeProcessor*>(&processor))
        ProbeHostDiagnostics::record(p->runtimeIdentity(), "channel", observeChannel(list).fields);
}
void ProbeHostDiagnostics::record(const juce::Uuid& runtime, const juce::String& kind, juce::var details)
{
    auto& s = sink(); const juce::ScopedLock lock(s.lock);
    auto value = object();
    put(value, "sequence", ++s.sequence); put(value, "runtime", runtime.toString());
    put(value, "kind", kind); put(value, "details", std::move(details));
    put(value, "utc", juce::Time::getCurrentTime().toISO8601(true));
    s.records[static_cast<size_t>((s.sequence - 1) % 256)] = std::move(value);
    if (s.sequence > 256) ++s.dropped;
}
juce::var ProbeHostDiagnostics::snapshot()
{
    auto& s = sink(); const juce::ScopedLock lock(s.lock);
    auto value = object(); juce::Array<juce::var> records;
    for (auto i = std::max(juce::int64(0), s.sequence - 256); i < s.sequence; ++i)
        records.add(s.records[static_cast<size_t>(i % 256)].clone());
    put(value, "records", records); put(value, "sequence", s.sequence); put(value, "dropped", s.dropped);
    put(value, "pid", s.pid); put(value, "process_start", s.start); put(value, "schema", 1);
    return value;
}
}
