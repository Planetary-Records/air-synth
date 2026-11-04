#include "ComfyHttpFixture.h"
#include "ComfyAudioJob.h"
#include "StrictJson.h"
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <limits>
#include <winhttp.h>

namespace airack::companion::detail {
struct ComfyHttpTestAccess {
  static std::unique_ptr<ComfyHttpWindows> fixture(ComfyLoopback kind, juce::File root, std::uint16_t port) {
    return std::unique_ptr<ComfyHttpWindows>(new ComfyHttpWindows(kind, std::move(root), port));
  }
  static void control(ComfyHttpWindows &h, ComfyTestControl *c) { h.setTestControl(c); }
  static std::array<unsigned, 2> live() { return ComfyHttpWindows::liveNetworkState(); }
};
}
namespace airack::drums::detail {
struct ComfyAudioJobTestAccess {
  static std::unique_ptr<ComfyAudioJob> make(std::unique_ptr<companion::ComfyHttpWindows> h) {
    return std::unique_ptr<ComfyAudioJob>(new ComfyAudioJob(std::move(h)));
  }
};
}
using namespace airack;
using namespace airack::drums;
using namespace airack::companion;
using namespace comfy_fixture;
namespace {
unsigned checks = 0, failures = 0;
const char *section = "setup";
bool check(bool pass, const char *expression, int line) {
  ++checks;
  if (!pass) { ++failures; std::cerr << section << ':' << line << ' ' << expression << '\n'; }
  return pass;
}
#define CHECK(x) check(bool(x), #x, __LINE__)
bool positiveControl = false, mismatchControl = false;
bool setupControls() {
  section = "fixture_setup_positive";
  NativeComfyFixture good("POST", "/fixture", "{}", response("control"));
  const std::string literal = "POST /fixture HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
  std::string got;
  positiveControl = good.valid() && independentExchange(good.port(), literal, got);
  good.join();
  positiveControl = positiveControl && good.captured().matched && good.requestCount() == 1 &&
    got == "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 7\r\nConnection: close\r\n\r\ncontrol";
  CHECK(positiveControl);
  section = "fixture_setup_mismatch";
  NativeComfyFixture bad("POST", "/expected", "expected", response("unreachable"));
  got.clear();
  mismatchControl = bad.valid() && independentExchange(bad.port(), literal, got);
  bad.join();
  mismatchControl = mismatchControl && bad.captured().complete && !bad.captured().matched &&
    bad.requestCount() == 1 && got == "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length: 8\r\nConnection: close\r\n\r\nmismatch";
  CHECK(mismatchControl);
  std::cout << "fixture_setup_positive=" << (positiveControl ? "PASS" : "FAIL")
            << " fixture_setup_mismatch=" << (mismatchControl ? "PASS" : "FAIL") << '\n';
  return positiveControl && mismatchControl;
}
struct Root {
  juce::File directory;
  bool valid = false;
  Root() {
    const auto *env = std::getenv("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT");
    if (!env || !juce::File::isAbsolutePath(juce::String::fromUTF8(env))) return;
    const juce::File parent(juce::String::fromUTF8(env));
    directory = parent.getChildFile("comfy-job-" + juce::Uuid().toString());
    valid = directory.isAChildOf(parent) && !directory.exists() && directory.createDirectory().wasOk();
  }
  ~Root() { if (valid) RemoveDirectoryW(directory.getFullPathName().toWideCharPointer()); }
};
ComfyAudioJobRequest makeRequest(std::array<std::uint64_t, 3> seeds = {17, 73, 211}) {
  ComfyAudioJobRequest request;
  request.runtime = juce::Uuid("00112233445566778899aabbccddeeff");
  request.requestUuid = juce::Uuid("abcdef0123456789abcdef0123456789");
  request.request = 42;
  request.prompt = "isolated kick, \"dry\" \\ caf\xc3\xa9";
  request.seeds = seeds;
  request.attempts = {juce::Uuid("102132435465768798a9bacbdcedfe0f"),
                      juce::Uuid("2031425364758697a8b9cadbecfd0e1f"),
                      juce::Uuid("30415263748596a7b8c9daebfc0d1e2f")};
  request.deadline = Clock::now() + std::chrono::seconds(5);
  return request;
}
std::string quote(std::string_view value) {
  return juce::JSON::toString(juce::var(juce::String::fromUTF8(value.data(), int(value.size()))), true).toStdString();
}
std::string parent(const ComfyAudioJobRequest &r, std::size_t i) {
  return "airack/" + r.runtime.toDashedString().toStdString() + "/" + std::to_string(r.request) + "/" + r.attempts[i].toDashedString().toStdString();
}
std::string frozenSubmit(const ComfyAudioJobRequest &r, std::size_t i) {
  return "{\"prompt_id\":" + quote(r.attempts[i].toDashedString().toStdString()) +
    ",\"client_id\":" + quote(r.requestUuid.toDashedString().toStdString()) + ",\"prompt\":{" +
    R"("1":{"class_type":"CheckpointLoaderSimple","inputs":{"ckpt_name":"stable_audio_3_small_sfx.safetensors"}},"2":{"class_type":"CLIPLoader","inputs":{"clip_name":"t5gemma_b_b_ul2.safetensors","type":"stable_audio","device":"default"}},"3":{"class_type":"CLIPTextEncode","inputs":{"clip":["2",0],"text":)" + quote(r.prompt) +
    R"(}},"8":{"class_type":"CLIPTextEncode","inputs":{"clip":["2",0],"text":""}},"4":{"class_type":"EmptyLatentAudio","inputs":{"seconds":10.0,"batch_size":1}},"5":{"class_type":"KSampler","inputs":{"model":["1",0],"positive":["3",0],"negative":["8",0],"latent_image":["4",0],"seed":)" + std::to_string(r.seeds[i]) +
    R"(,"steps":8,"cfg":1.0,"sampler_name":"lcm","scheduler":"simple","denoise":1.0}},"6":{"class_type":"VAEDecodeAudio","inputs":{"samples":["5",0],"vae":["1",2]}},"7":{"class_type":"SaveAudioAdvanced","inputs":{"audio":["6",0],"filename_prefix":)" + quote(parent(r, i) + "/seed-" + std::to_string(r.seeds[i])) + R"(,"format":"flac"}}}})";
}
std::string successHistory(const ComfyAudioJobRequest &r, std::size_t i) {
  return "{" + quote(r.attempts[i].toDashedString().toStdString()) +
    ":{\"status\":{\"completed\":true,\"status_str\":\"success\"},\"outputs\":{\"7\":{\"audio\":[{\"filename\":" + quote("seed-" + std::to_string(r.seeds[i]) + "_00001.flac") +
    ",\"subfolder\":" + quote(parent(r, i)) + ",\"type\":\"output\"}]}}}}";
}
std::string errorHistory(const ComfyAudioJobRequest &r, std::size_t i) {
  return "{" + quote(r.attempts[i].toDashedString().toStdString()) + ":{\"status\":{\"completed\":false,\"status_str\":\"error\"}}}";
}
std::string encodedParent(std::string value) {
  std::string result;
  for (char c : value) { if (c == '/') result += "%2F"; else result += c; }
  return result;
}
std::string downloadTarget(const ComfyAudioJobRequest &r, std::size_t i) {
  return "/view?filename=seed-" + std::to_string(r.seeds[i]) + "_00001.flac&subfolder=" + encodedParent(parent(r, i)) + "&type=output";
}
std::string validFlac() {
  juce::MemoryBlock block;
  std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream>(block, false);
  juce::FlacAudioFormat format;
  auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(44100).withNumChannels(2).withBitsPerSample(16));
  juce::AudioBuffer<float> audio(2, 442368);
  audio.clear();
  for (int i = 0; i != audio.getNumSamples(); ++i) {
    audio.setSample(0, i, float((i % 67) - 33) / 32768.0f);
    audio.setSample(1, i, float((i % 53) - 26) / 32768.0f);
  }
  if (!writer || !writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples())) return {};
  writer.reset();
  return {static_cast<const char *>(block.getData()), block.getSize()};
}
const std::string rejectEnvelope = R"({"error":{"type":"prompt_outputs_failed_validation","message":"Prompt outputs failed validation","details":"fixture validation failure","extra_info":{}},"node_errors":{"7":{"errors":[{"type":"required_input_missing","message":"Required input is missing","details":"audio","extra_info":{}}],"dependent_outputs":["7"],"class_type":"SaveAudioAdvanced"}}})";
struct Admission {
  std::atomic_bool current{true};
  std::array<std::atomic_uint, 9> calls{};
  int rejectOrdinal = -1, rejectBoundary = -1;
  unsigned rejectCall = 0;
  ComfyJobAdmission get() {
    return {[](void *p, ComfyAuthorityBoundary b, std::size_t i) noexcept {
      auto &a = *static_cast<Admission *>(p);
      const auto n = i < 3 ? ++a.calls[i * 3 + std::size_t(b)] : 0;
      if (int(i) == a.rejectOrdinal && int(b) == a.rejectBoundary && n == a.rejectCall) return false;
      return a.current.load();
    }, this};
  }
};
struct ThreeFixture {
  const ComfyAudioJobRequest &request;
  juce::File root;
  std::string flac;
  Admission admission;
  std::unique_ptr<NativeComfyFixture> server;
  ThreeFixture(const ComfyAudioJobRequest &r, juce::File directory, bool rejection = false)
      : request(r), root(std::move(directory)), flac(validFlac()) {
    FixtureOptions options;
    for (std::size_t i = 0; i != 3; ++i) {
      const auto id = r.attempts[i].toDashedString().toStdString();
      options.script.push_back({"POST", "/prompt", frozenSubmit(r, i), response(rejection ? rejectEnvelope : "{\"prompt_id\":" + quote(id) + ",\"number\":0,\"node_errors\":{}}", "application/json", rejection ? 400 : 200)});
      options.script.push_back({"GET", "/history/" + id, "", response(rejection ? errorHistory(r, i) : "{}")});
      if (rejection) break;
      options.script.push_back({"GET", "/history/" + id, "", response(successHistory(r, i))});
      options.script.push_back({"GET", downloadTarget(r, i), "", response(flac, "audio/flac")});
    }
    server = std::make_unique<NativeComfyFixture>("", "", "", "", std::move(options));
  }
  std::unique_ptr<ComfyAudioJob> makeJob() {
    return drums::detail::ComfyAudioJobTestAccess::make(companion::detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root, server->port()));
  }
  bool setupPositivePassed() const { return positiveControl; }
  bool setupMismatchPassed() const { return mismatchControl; }
  ComfyJobAdmission alwaysCurrentAdmission() { return admission.get(); }
  unsigned posts() const { unsigned n = 0; for (const auto &c : server->captures()) n += c.method == "POST"; return n; }
  unsigned foreignRequests() const { unsigned n = 0; for (const auto &c : server->captures()) n += !c.matched; return n; }
  std::size_t flacBytes() const { return flac.size(); }
  bool firstSubmitGraphEqualsFrozenGraph(const ComfyAudioJobRequest &r, std::size_t i) const {
    const auto captures = server->captures();
    return !captures.empty() && captures.front().body == frozenSubmit(r, i);
  }
};
std::unique_ptr<ThreeFixture> runThreeFixture(const ComfyAudioJobRequest &r, const juce::File &root, bool rejection = false) {
  return std::make_unique<ThreeFixture>(r, root, rejection);
}
void three(const juce::File &root) {
  section = "three_real_candidates";
  auto request = makeRequest();
  auto fixture = runThreeFixture(request, root);
  CHECK(fixture->server->valid() && fixture->flac.size() > 42 && fixture->flac.starts_with("fLaC"));
  juce::FlacAudioFormat format;
  std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::MemoryInputStream(fixture->flac.data(), fixture->flac.size(), false), true));
  CHECK(reader && reader->sampleRate == 44100 && reader->numChannels == 2 && reader->bitsPerSample == 16 && reader->lengthInSamples == 442368);
  auto job = fixture->makeJob();
  auto batch = job->run(request, fixture->alwaysCurrentAdmission());
  fixture->server->join();
  CHECK(fixture->setupPositivePassed() && fixture->setupMismatchPassed());
  CHECK(fixture->posts() == 3 && fixture->foreignRequests() == 0);
  CHECK(batch.status == DrumGenerationStatus::complete && batch.rawCount == 3);
  CHECK(batch.attempts[0].raw && batch.attempts[0].raw->size() == fixture->flacBytes());
  CHECK(fixture->firstSubmitGraphEqualsFrozenGraph(request, 0));
  for (std::size_t i = 0; i != 3; ++i) {
    CHECK(batch.attempts[i].owned.prompt == request.attempts[i] && batch.attempts[i].owned.seed == request.seeds[i]);
    if (batch.attempts[i].raw) {
      const auto hash = juce::SHA256(fixture->flac.data(), fixture->flac.size()).getRawData();
      CHECK(batch.attempts[i].encodedBytes == fixture->flac.size() && std::memcmp(batch.attempts[i].fileSha256.data(), hash.getData(), 32) == 0);
    }
  }
}
void rejection(const juce::File &root) {
  section = "real_http400_rejection_no_history";
  auto request = makeRequest();
  auto fixture = runThreeFixture(request, root, true);
  auto job = fixture->makeJob();
  auto batch = job->run(request, fixture->alwaysCurrentAdmission());
  fixture->server->join();
  CHECK(fixture->posts() == 1 && fixture->server->requestCount() == 1 && fixture->foreignRequests() == 0);
  CHECK(batch.status == DrumGenerationStatus::recipeRejected && batch.rawCount == 0);
  CHECK(batch.attempts[0].status == ComfyAttemptStatus::rejected && batch.attempts[1].status == ComfyAttemptStatus::unrun && batch.attempts[2].status == ComfyAttemptStatus::unrun);
}
std::string accepted(const ComfyAudioJobRequest &r, std::size_t i) {
  return "{\"prompt_id\":" + quote(r.attempts[i].toDashedString().toStdString()) + ",\"number\":0,\"node_errors\":{}}";
}
void addCandidate(FixtureOptions &options, const ComfyAudioJobRequest &r, std::size_t i, const std::string &flac) {
  options.script.push_back({"POST", "/prompt", frozenSubmit(r, i), response(accepted(r, i))});
  options.script.push_back({"GET", "/history/" + r.attempts[i].toDashedString().toStdString(), "", response(successHistory(r, i))});
  options.script.push_back({"GET", downloadTarget(r, i), "", response(flac, "audio/flac")});
}
std::unique_ptr<ComfyAudioJob> makeJob(NativeComfyFixture &server, const juce::File &root, companion::detail::ComfyTestControl *control = nullptr) {
  auto transport = companion::detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root, server.port());
  if (control) companion::detail::ComfyHttpTestAccess::control(*transport, control);
  return drums::detail::ComfyAudioJobTestAccess::make(std::move(transport));
}
void codecs(const juce::File &root) {
  section = "fixed_graph_and_request_validation";
  auto request = makeRequest({0, 17, std::uint64_t(std::numeric_limits<std::int64_t>::max())});
  for (std::size_t i = 0; i != 3; ++i) {
    std::string body = "preserved";
    CHECK(buildComfyAudioSubmit(request, i, body));
    CHECK(body == frozenSubmit(request, i) && body.size() <= 16384);
    juce::var decoded;
    CHECK(ai::parseStrictJson(body, decoded, 16384));
    CHECK(decoded["prompt"].getDynamicObject()->getProperties().size() == 8);
    CHECK(decoded["prompt"]["5"]["inputs"]["seed"].isInt64() || decoded["prompt"]["5"]["inputs"]["seed"].isInt());
    CHECK(std::uint64_t(juce::int64(decoded["prompt"]["5"]["inputs"]["seed"])) == request.seeds[i]);
  }
  request.prompt = std::string(2048, '\\');
  std::string body;
  CHECK(buildComfyAudioSubmit(request, 0, body) && body == frozenSubmit(request, 0) && body.size() <= 16384);
  for (unsigned kind = 0; kind != 15; ++kind) {
    request = makeRequest();
    switch (kind) {
    case 0: request.prompt.clear(); break;
    case 1: request.prompt.assign(2049, 'x'); break;
    case 2: request.prompt = std::string("bad\0text", 8); break;
    case 3: request.prompt = "line\nfeed"; break;
    case 4: request.prompt = std::string("\xc3", 1); break;
    case 5: request.prompt = "\x7f"; break;
    case 6: request.runtime = juce::Uuid::null(); break;
    case 7: request.requestUuid = juce::Uuid::null(); break;
    case 8: request.attempts[0] = juce::Uuid::null(); break;
    case 9: request.attempts[1] = request.attempts[0]; break;
    case 10: request.seeds[1] = request.seeds[0]; break;
    case 11: request.seeds[2] = std::uint64_t(std::numeric_limits<std::int64_t>::max()) + 1; break;
    case 12: request.deadline = Clock::now(); break;
    case 13: request.deadline = Clock::now() + std::chrono::seconds(301); break;
    case 14: request.request = 0; break;
    }
    body = "preserved";
    CHECK(!buildComfyAudioSubmit(request, 0, body) && body == "preserved");
    NativeComfyFixture server("POST", "/must-not-connect", "", response("{}"));
    auto job = makeJob(server, root);
    Admission admission;
    const auto batch = job->run(request, admission.get());
    server.join();
    CHECK(batch.status == DrumGenerationStatus::invalidRequest && batch.rawCount == 0 && server.requestCount() == 0);
  }
  request = makeRequest(); body = "preserved";
  CHECK(!buildComfyAudioSubmit(request, 3, body) && body == "preserved");
  NativeComfyFixture noAdmission("POST", "/must-not-connect", "", response("{}"));
  auto job = makeJob(noAdmission, root);
  CHECK(job->run(request, {}).status == DrumGenerationStatus::invalidRequest);
  noAdmission.join(); CHECK(noAdmission.requestCount() == 0);

  section = "strict_owned_history_codec";
  const ComfyOwnedAttempt owned{request.runtime, request.attempts[0], request.request, request.seeds[0]};
  ComfyOwnedOutput output{"preserved-file", "preserved-folder"};
  CHECK(parseOwnedComfyHistory("{}", owned, output) == OwnedHistoryStatus::absent && output.filename == "preserved-file");
  CHECK(parseOwnedComfyHistory(errorHistory(request, 0), owned, output) == OwnedHistoryStatus::remoteFailed && output.filename == "preserved-file");
  const auto proper = successHistory(request, 0);
  CHECK(parseOwnedComfyHistory(proper, owned, output) == OwnedHistoryStatus::success && output.filename == "seed-17_00001.flac" && output.subfolder == parent(request, 0));
  // The pinned Windows saver returns os.path.normpath's exact owned parent.
  auto windowsParent = parent(request, 0);
  std::replace(windowsParent.begin(), windowsParent.end(), '/', '\\');
  auto windowsHistory = proper;
  const auto folderAt = windowsHistory.find(quote(parent(request, 0)));
  CHECK(folderAt != std::string::npos);
  windowsHistory.replace(folderAt, quote(parent(request, 0)).size(), quote(windowsParent));
  output = {"preserved-file", "preserved-folder"};
  CHECK(parseOwnedComfyHistory(windowsHistory, owned, output) == OwnedHistoryStatus::success && output.filename == "seed-17_00001.flac" && output.subfolder == parent(request, 0));
  for (const auto &invalidParent : std::vector<std::string>{"\\\\" + windowsParent, windowsParent + "\\..", "C:\\" + windowsParent,
       "airack/" + windowsParent.substr(7), windowsParent + "%2f", windowsParent + "\\", windowsParent + "\\foreign"}) {
    auto invalid = proper;
    invalid.replace(folderAt, quote(parent(request, 0)).size(), quote(invalidParent));
    output = {"preserved-file", "preserved-folder"};
    CHECK(parseOwnedComfyHistory(invalid, owned, output) == OwnedHistoryStatus::invalid && output.filename == "preserved-file" && output.subfolder == "preserved-folder");
  }
  std::vector<std::string> bad = {"null", "[]", "{", proper + "x", "{\"a\":1,\"a\":2}", "{\"status\":{\"completed\":true,\"status_str\":\"success\"}}"};
  const auto replace = [&](std::string from, std::string to) {
    auto value = proper; const auto p = value.find(from); CHECK(p != std::string::npos); if (p != std::string::npos) value.replace(p, from.size(), to); bad.push_back(std::move(value));
  };
  replace(request.attempts[0].toDashedString().toStdString(), request.attempts[1].toDashedString().toStdString());
  replace("\"completed\":true", "\"completed\":false");
  replace("\"completed\":true", "\"completed\":null");
  replace("\"status_str\":\"success\"", "\"status_str\":\"error\"");
  replace("\"7\":", "\"6\":");
  replace("\"outputs\":{", "\"outputs\":{\"8\":{},");
  replace("\"audio\":[", "\"audio\":[{},");
  replace("\"type\":\"output\"", "\"type\":null");
  replace("\"type\":\"output\"", "\"type\":\"input\"");
  replace("seed-17_00001.flac", "../seed-17_00001.flac");
  replace("seed-17_00001.flac", "seed-17_%30.flac");
  replace("seed-17_00001.flac", "seed-17_000000000000000000000.flac");
  replace("seed-17_00001.flac", "seed-18_00001.flac");
  replace("seed-17_00001.flac", "C:\\\\seed-17_00001.flac");
  replace(parent(request, 0), parent(request, 1));
  replace("\"filename\":", "\"url\":\"http://foreign/\",\"filename\":");
  bad.push_back(std::string("{\"bad\":\"\0\"}", 11));
  bad.push_back("{\"bad\":\"\xc3\"}");
  bad.push_back(std::string(25, '[') + "0" + std::string(25, ']'));
  bad.push_back("{" + quote(request.attempts[0].toDashedString().toStdString()) + ":null}");
  bad.push_back("{" + quote(request.attempts[0].toDashedString().toStdString()) + ":{},\"foreign\":{}}");
  for (const auto &value : bad) {
    output = {"preserved-file", "preserved-folder"};
    CHECK(parseOwnedComfyHistory(value, owned, output) == OwnedHistoryStatus::invalid && output.filename == "preserved-file" && output.subfolder == "preserved-folder");
  }
  auto maximum = proper + std::string(262144 - proper.size(), ' ');
  CHECK(maximum.size() == 262144);
  CHECK(parseOwnedComfyHistory(maximum, owned, output) == OwnedHistoryStatus::success);
  maximum += ' '; CHECK(parseOwnedComfyHistory(maximum, owned, output) == OwnedHistoryStatus::invalid);
}
void states(const juce::File &root) {
  section = "owned_uuid_ambiguity_and_partial";
  const auto flac = validFlac();
  for (unsigned kind = 0; kind != 8; ++kind) {
    auto request = makeRequest();
    FixtureOptions options;
    if (kind == 5 || kind == 6) addCandidate(options, request, 0, flac);
    const std::size_t i = kind == 5 || kind == 6 ? 1 : 0;
    std::string submit = response(accepted(request, i));
    if (kind == 0) submit.clear();
    if (kind == 1) submit = response("{\"prompt_id\":" + quote(request.attempts[2].toDashedString().toStdString()) + ",\"number\":0,\"node_errors\":{}}");
    if (kind == 2) submit = response("{}", "application/json", 400);
    if (kind == 3) submit = response(rejectEnvelope, "application/json", 500);
    if (kind == 4 || kind == 5) submit = response(rejectEnvelope, "application/json", 400);
    if (kind == 7) submit = response("{\"prompt_id\":" + quote(request.attempts[i].toDashedString().toStdString()) + ",\"number\":0.5,\"node_errors\":{}}");
    options.script.push_back({"POST", "/prompt", frozenSubmit(request, i), submit});
    if (kind != 4 && kind != 5) {
      options.script.push_back({"GET", "/history/" + request.attempts[i].toDashedString().toStdString(), "", response(kind == 0 ? successHistory(request, i) : errorHistory(request, i))});
      if (kind == 0) {
        options.script.push_back({"GET", downloadTarget(request, i), "", response(flac, "audio/flac")});
        addCandidate(options, request, 1, flac); addCandidate(options, request, 2, flac);
      }
    }
    NativeComfyFixture server("", "", "", "", std::move(options));
    auto job = makeJob(server, root); Admission admission;
    auto batch = job->run(request, admission.get()); server.join();
    for (const auto &c : server.captures()) CHECK(c.matched && c.target.find("/cancel") == std::string::npos);
    unsigned posts = 0; for (const auto &c : server.captures()) posts += c.method == "POST";
    if (kind == 0) CHECK(batch.status == DrumGenerationStatus::complete && batch.rawCount == 3 && posts == 3 && server.requestCount() == 9);
    else if (kind == 4) CHECK(batch.status == DrumGenerationStatus::recipeRejected && posts == 1 && server.requestCount() == 1);
    else if (kind == 5 || kind == 6) CHECK(batch.status == DrumGenerationStatus::partial && batch.rawCount == 1 && batch.attempts[0].raw && batch.attempts[1].status == (kind == 5 ? ComfyAttemptStatus::rejected : ComfyAttemptStatus::remoteFailed) && batch.attempts[2].status == ComfyAttemptStatus::unrun && posts == 2);
    else CHECK(batch.status == DrumGenerationStatus::remoteFailed && batch.rawCount == 0 && posts == 1 && server.requestCount() == 2);
  }
  section = "authority_at_every_real_http_boundary";
  for (int ordinal = 0; ordinal != 3; ++ordinal) for (int boundary = 0; boundary != 3; ++boundary) for (unsigned call : {1u, 2u}) {
    auto request = makeRequest(); FixtureOptions options;
    for (std::size_t i = 0; i != 3; ++i) addCandidate(options, request, i, flac);
    NativeComfyFixture server("", "", "", "", std::move(options));
    auto job = makeJob(server, root); Admission admission;
    admission.rejectOrdinal = ordinal; admission.rejectBoundary = boundary; admission.rejectCall = call;
    auto batch = job->run(request, admission.get()); server.join();
    CHECK(batch.status == DrumGenerationStatus::stale && batch.rawCount == 0);
    CHECK(server.requestCount() == unsigned(ordinal * 3 + boundary + (call == 2 ? 1 : 0)));
    for (const auto &attempt : batch.attempts) CHECK(!attempt.raw);
    for (const auto &c : server.captures()) CHECK(c.matched);
    CHECK(root.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty());
  }
  section = "invalid_owned_history_never_downloads";
  for (unsigned kind = 0; kind != 8; ++kind) {
    auto request = makeRequest(); auto body = successHistory(request, 0);
    std::string from, to;
    switch (kind) {
    case 0: from = request.attempts[0].toDashedString().toStdString(); to = request.attempts[1].toDashedString().toStdString(); break;
    case 1: from = "\"outputs\":{"; to = "\"outputs\":{\"8\":{},"; break;
    case 2: from = "seed-17_00001.flac"; to = "../seed-17_00001.flac"; break;
    case 3: from = "\"completed\":true"; to = "\"completed\":false"; break;
    case 4: from = "\"audio\":["; to = "\"audio\":[{},"; break;
    case 5: from = "\"7\":"; to = "\"6\":"; break;
    case 6: body = "{\"status\":{\"completed\":true,\"status_str\":\"success\"}}"; break;
    case 7: body = "{" + quote(request.attempts[0].toDashedString().toStdString()) + ":null}"; break;
    }
    if (!from.empty()) { const auto p = body.find(from); CHECK(p != std::string::npos); body.replace(p, from.size(), to); }
    FixtureOptions options;
    options.script.push_back({"POST", "/prompt", frozenSubmit(request, 0), response(accepted(request, 0))});
    options.script.push_back({"GET", "/history/" + request.attempts[0].toDashedString().toStdString(), "", response(body)});
    NativeComfyFixture server("", "", "", "", std::move(options));
    auto job = makeJob(server, root); Admission admission;
    auto batch = job->run(request, admission.get()); server.join();
    CHECK(batch.status == DrumGenerationStatus::invalidResponse && !batch.rawCount && batch.attempts[1].status == ComfyAttemptStatus::unrun);
    CHECK(server.requestCount() == 2); for (const auto &c : server.captures()) CHECK(c.matched && !c.target.starts_with("/view"));
  }
}
void lifecycle(const juce::File &root) {
  section = "cancel_before_submit_and_during_send_download";
  const auto flac = validFlac();
  for (unsigned point = 0; point != 3; ++point) {
    auto request = makeRequest(); FixtureOptions options; addCandidate(options, request, 0, flac);
    Barrier barrier; std::atomic_uint count{0};
    options.beforeReply = [&] { const auto n = ++count; if ((point == 1 && n == 1) || (point == 2 && n == 3)) barrier.reach(); };
    NativeComfyFixture server("", "", "", "", std::move(options));
    auto job = makeJob(server, root); Admission admission;
    if (!point) job->cancel();
    auto running = std::async(std::launch::async, [&] { return job->run(request, admission.get()); });
    if (point) { CHECK(barrier.waitReached()); job->cancel(); barrier.release(); }
    auto batch = running.get(); server.join();
    CHECK(batch.status == DrumGenerationStatus::cancelled && batch.rawCount == 0);
    CHECK(server.requestCount() == (point == 0 ? 0u : point == 1 ? 1u : 3u));
    for (const auto &c : server.captures()) CHECK(c.matched && c.target.find("cancel") == std::string::npos);
    CHECK(root.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty());
  }
  section = "cancel_absent_wait_and_original_deadline";
  for (unsigned stop = 0; stop != 3; ++stop) {
    auto request = makeRequest(); request.deadline = Clock::now() + std::chrono::milliseconds(350);
    // Repeated absent-history GETs have the same exact request and no new POST.
    FixtureOptions options;
    options.script.push_back({"POST", "/prompt", frozenSubmit(request, 0), response(stop == 2 ? "{}" : accepted(request, 0))});
    for (unsigned n = 0; n != 8; ++n) options.script.push_back({"GET", "/history/" + request.attempts[0].toDashedString().toStdString(), "", response("{}")});
    NativeComfyFixture server("", "", "", "", std::move(options));
    auto job = makeJob(server, root); Admission admission;
    auto running = std::async(std::launch::async, [&] { return job->run(request, admission.get()); });
    if (!stop) {
      const auto end = Clock::now() + std::chrono::seconds(2);
      while (admission.calls[1] < 2 && Clock::now() < end) std::this_thread::yield();
      CHECK(admission.calls[1] >= 2); job->cancel();
    }
    auto batch = running.get(); server.join();
    CHECK(batch.status == (stop == 0 ? DrumGenerationStatus::cancelled : stop == 1 ? DrumGenerationStatus::timeout : DrumGenerationStatus::submissionAmbiguous));
    CHECK(batch.rawCount == 0 && batch.attempts[1].status == ComfyAttemptStatus::unrun && batch.attempts[2].status == ComfyAttemptStatus::unrun);
    unsigned posts = 0; for (const auto &c : server.captures()) { posts += c.method == "POST"; CHECK(c.matched); }
    CHECK(posts == 1 && server.requestCount() <= 6);
  }
  section = "currentness_after_actual_final_callback_drain";
  for (unsigned stop = 0; stop != 3; ++stop) {
    auto request = makeRequest(); request.deadline = Clock::now() + std::chrono::milliseconds(stop == 2 ? 350 : 3000);
    FixtureOptions options; addCandidate(options, request, 0, flac);
    NativeComfyFixture server("", "", "", "", std::move(options));
    struct Context { Barrier barrier; std::atomic_bool selected{false}, held{false}; } context;
    companion::detail::ComfyTestControl control; control.context = &context;
    control.networkPoint = [](void *p, companion::detail::ComfyNetworkTestPoint point, unsigned code) noexcept {
      auto &c = *static_cast<Context *>(p);
      if (point == companion::detail::ComfyNetworkTestPoint::resultSelected) c.selected = true;
      if (point == companion::detail::ComfyNetworkTestPoint::callbackLeaving && code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING && c.selected && !c.held.exchange(true)) c.barrier.reach();
    };
    auto job = makeJob(server, root, &control); Admission admission;
    auto running = std::async(std::launch::async, [&] { return job->run(request, admission.get()); });
    CHECK(context.barrier.waitReached());
    CHECK(running.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    if (stop == 0) admission.current = false;
    if (stop == 1) job->cancel();
    if (stop == 2) {
      while (Clock::now() < request.deadline) std::this_thread::yield();
    }
    context.barrier.release();
    auto batch = running.get(); server.join();
    CHECK(batch.status == (stop == 0 ? DrumGenerationStatus::stale : stop == 1 ? DrumGenerationStatus::cancelled : DrumGenerationStatus::timeout));
    CHECK(batch.rawCount == 0 && server.requestCount() == 1);
  }
  section = "original_deadline_after_two_candidates_preserves_partial";
  auto request = makeRequest(); request.deadline = Clock::now() + std::chrono::milliseconds(1500);
  FixtureOptions options; addCandidate(options, request, 0, flac); addCandidate(options, request, 1, flac); addCandidate(options, request, 2, flac);
  Barrier thirdSubmit; std::atomic_uint replies{0};
  options.beforeReply = [&] { if (++replies == 7) thirdSubmit.reach(); };
  NativeComfyFixture server("", "", "", "", std::move(options));
  auto job = makeJob(server, root); Admission admission;
  auto running = std::async(std::launch::async, [&] { return job->run(request, admission.get()); });
  CHECK(thirdSubmit.waitReached());
  while (Clock::now() < request.deadline) std::this_thread::yield();
  thirdSubmit.release();
  auto batch = running.get(); server.join();
  CHECK(batch.status == DrumGenerationStatus::partial && batch.rawCount == 2 && batch.attempts[0].raw && batch.attempts[1].raw && !batch.attempts[2].raw && batch.attempts[2].status == ComfyAttemptStatus::timeout);
  CHECK(server.requestCount() == 7);
}
}
int main(int argc, char **argv) {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
  struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
  std::string selected = "all";
  if (argc == 3 && std::string(argv[1]) == "--section") selected = argv[2];
  else if (argc != 1) return 2;
  if (selected != "all" && selected != "red" && selected != "rejection" && selected != "codecs" && selected != "states" && selected != "lifecycle") return 2;
  if (!setupControls()) { std::cerr << "SETUP_FAILURE: not behavioral RED\n"; return 2; }
  Root root;
  if (!root.valid) { std::cerr << "SETUP_FAILURE: diagnostic root\n"; return 2; }
  if (selected == "all" || selected == "red") three(root.directory);
  if (selected == "all" || selected == "rejection") rejection(root.directory);
  if (selected == "all" || selected == "codecs") codecs(root.directory);
  if (selected == "all" || selected == "states") states(root.directory);
  if (selected == "all" || selected == "lifecycle") lifecycle(root.directory);
  CHECK(companion::detail::ComfyHttpTestAccess::live() == (std::array<unsigned, 2>{0, 0}));
  CHECK(root.directory.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty());
  std::cout << "Comfy audio job native: " << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
