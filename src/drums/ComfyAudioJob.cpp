#include "ComfyAudioJob.h"
#include "StrictJson.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace airack::drums {
namespace {
using HttpStatus = companion::ComfyHttpStatus;
using Attempt = ComfyAttemptStatus;
using Status = DrumGenerationStatus;
constexpr std::size_t jsonLimit = 262144, binaryLimit = 16777216;
bool validRequest(const ComfyAudioJobRequest &r) noexcept {
  const auto now = GenerationClock::now();
  if (r.runtime.isNull() || r.requestUuid.isNull() || !r.request || r.prompt.empty() || r.prompt.size() > 2048 ||
      !juce::CharPointer_UTF8::isValidString(r.prompt.data(), int(r.prompt.size())) || r.prompt.find('\0') != std::string::npos ||
      r.deadline <= now || r.deadline - now > std::chrono::seconds(300)) return false;
  auto text = juce::CharPointer_UTF8(r.prompt.c_str());
  while (!text.isEmpty()) { const auto c = text.getAndAdvance(); if (c < 32 || (c >= 127 && c <= 159)) return false; }
  for (std::size_t i = 0; i != 3; ++i) {
    if (r.attempts[i].isNull() || r.seeds[i] > std::uint64_t(std::numeric_limits<std::int64_t>::max())) return false;
    for (std::size_t j = 0; j < i; ++j) if (r.attempts[i] == r.attempts[j] || r.seeds[i] == r.seeds[j]) return false;
  }
  return true;
}
std::string decimal(std::uint64_t value) {
  std::array<char, 20> bytes{};
  const auto end = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
  return {bytes.data(), end.ptr};
}
std::string jsonQuote(std::string_view text) {
  return juce::JSON::toString(juce::var(juce::String::fromUTF8(text.data(), int(text.size()))), true).toStdString();
}
std::string outputParent(const companion::ComfyOwnedAttempt &owned) {
  return "airack/" + owned.runtime.toDashedString().toStdString() + "/" + decimal(owned.request) + "/" + owned.prompt.toDashedString().toStdString();
}
std::string submitBody(const ComfyAudioJobRequest &r, std::size_t i) {
  const companion::ComfyOwnedAttempt owned{r.runtime, r.attempts[i], r.request, r.seeds[i]};
  return "{\"prompt_id\":" + jsonQuote(r.attempts[i].toDashedString().toStdString()) +
    ",\"client_id\":" + jsonQuote(r.requestUuid.toDashedString().toStdString()) + ",\"prompt\":{" +
    R"("1":{"class_type":"CheckpointLoaderSimple","inputs":{"ckpt_name":"stable_audio_3_small_sfx.safetensors"}},"2":{"class_type":"CLIPLoader","inputs":{"clip_name":"t5gemma_b_b_ul2.safetensors","type":"stable_audio","device":"default"}},"3":{"class_type":"CLIPTextEncode","inputs":{"clip":["2",0],"text":)" + jsonQuote(r.prompt) +
    R"(}},"8":{"class_type":"CLIPTextEncode","inputs":{"clip":["2",0],"text":""}},"4":{"class_type":"EmptyLatentAudio","inputs":{"seconds":10.0,"batch_size":1}},"5":{"class_type":"KSampler","inputs":{"model":["1",0],"positive":["3",0],"negative":["8",0],"latent_image":["4",0],"seed":)" + decimal(r.seeds[i]) +
    R"(,"steps":8,"cfg":1.0,"sampler_name":"lcm","scheduler":"simple","denoise":1.0}},"6":{"class_type":"VAEDecodeAudio","inputs":{"samples":["5",0],"vae":["1",2]}},"7":{"class_type":"SaveAudioAdvanced","inputs":{"audio":["6",0],"filename_prefix":)" + jsonQuote(outputParent(owned) + "/seed-" + decimal(r.seeds[i])) + R"(,"format":"flac"}}}})";
}
bool validSubmitBody(const std::string &body) {
  juce::var decoded;
  return body.size() <= 16384 && ai::parseStrictJson(body, decoded, 16384);
}
bool validFilename(std::string_view filename, std::uint64_t seed) {
  const auto prefix = "seed-" + decimal(seed) + "_";
  if (!filename.starts_with(prefix) || !filename.ends_with(".flac") || filename.size() <= prefix.size() + 5) return false;
  const auto digits = filename.substr(prefix.size(), filename.size() - prefix.size() - 5);
  return digits.size() <= 20 && std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
}
OwnedHistoryStatus history(std::string_view text, const companion::ComfyOwnedAttempt &owned, ComfyOwnedOutput &destination) {
  juce::var decoded;
  if (owned.runtime.isNull() || owned.prompt.isNull() || !owned.request || !ai::parseStrictJson(text, decoded, jsonLimit)) return OwnedHistoryStatus::invalid;
  const auto *outer = decoded.getDynamicObject();
  if (!outer) return OwnedHistoryStatus::invalid;
  const auto &properties = outer->getProperties();
  if (properties.isEmpty()) return OwnedHistoryStatus::absent;
  if (properties.size() != 1 || properties.getName(0).toString() != owned.prompt.toDashedString()) return OwnedHistoryStatus::invalid;
  const auto &entry = properties.getValueAt(0);
  if (!entry.isObject() || !entry["status"].isObject()) return OwnedHistoryStatus::invalid;
  const auto &status = entry["status"];
  const auto &completed = status["completed"], &name = status["status_str"];
  if (!completed.isBool() || !name.isString()) return OwnedHistoryStatus::invalid;
  if (!bool(completed) && name.toString() == "error") return OwnedHistoryStatus::remoteFailed;
  if (!bool(completed) || name.toString() != "success") return OwnedHistoryStatus::invalid;
  const auto *outputs = entry["outputs"].getDynamicObject();
  if (!outputs || outputs->getProperties().size() != 1 || outputs->getProperties().getName(0).toString() != "7") return OwnedHistoryStatus::invalid;
  const auto &node = outputs->getProperties().getValueAt(0);
  if (!node.isObject()) return OwnedHistoryStatus::invalid;
  const auto *audio = node["audio"].getArray();
  if (!audio || audio->size() != 1) return OwnedHistoryStatus::invalid;
  const auto &item = audio->getReference(0);
  const auto *object = item.getDynamicObject();
  if (!object || object->getProperties().size() != 3 || !item["filename"].isString() || !item["subfolder"].isString() ||
      !item["type"].isString() || item["type"].toString() != "output") return OwnedHistoryStatus::invalid;
  ComfyOwnedOutput candidate{item["filename"].toString().toStdString(), item["subfolder"].toString().toStdString()};
  if (!validFilename(candidate.filename, owned.seed)) return OwnedHistoryStatus::invalid;
  auto expected = outputParent(owned);
  if (candidate.subfolder != expected) {
    // The pinned Windows saver applies os.path.normpath to this exact prefix.
    auto windowsParent = expected;
    std::replace(windowsParent.begin(), windowsParent.end(), '/', '\\');
    if (candidate.subfolder != windowsParent) return OwnedHistoryStatus::invalid;
    candidate.subfolder.swap(expected);
  }
  destination = std::move(candidate);
  return OwnedHistoryStatus::success;
}
bool acceptedSubmit(std::string_view body, const juce::Uuid &owned) {
  juce::var decoded;
  if (!ai::parseStrictJson(body, decoded, jsonLimit) || !decoded.isObject() || !decoded["prompt_id"].isString() ||
      decoded["prompt_id"].toString() != owned.toDashedString()) return false;
  const auto &number = decoded["number"];
  if (!number.isInt() && !number.isInt64() && !number.isDouble()) return false;
  const double numeric = double(number);
  const auto *errors = decoded["node_errors"].getDynamicObject();
  return std::isfinite(numeric) && numeric >= 0 && std::floor(numeric) == numeric && errors && errors->getProperties().isEmpty();
}
Attempt httpFailure(HttpStatus status) noexcept {
  switch (status) {
  case HttpStatus::cancelled: return Attempt::cancelled;
  case HttpStatus::timeout: return Attempt::timeout;
  case HttpStatus::storageFailure: return Attempt::storageFailure;
  case HttpStatus::unavailable: return Attempt::unavailable;
  default: return Attempt::invalidResponse;
  }
}
Status terminal(Attempt status) noexcept {
  switch (status) {
  case Attempt::rejected: return Status::recipeRejected;
  case Attempt::submissionAmbiguous: return Status::submissionAmbiguous;
  case Attempt::remoteFailed: return Status::remoteFailed;
  case Attempt::unavailable: return Status::unavailable;
  case Attempt::timeout: return Status::timeout;
  case Attempt::cancelled: return Status::cancelled;
  case Attempt::storageFailure: return Status::storageFailure;
  case Attempt::allocationFailed: return Status::allocationFailed;
  case Attempt::stale: return Status::stale;
  default: return Status::invalidResponse;
  }
}
}
ComfyAudioJob::ComfyAudioJob(companion::ComfyLoopback loopback, juce::File root)
    : ComfyAudioJob(std::make_unique<companion::ComfyHttpWindows>(loopback, std::move(root))) {}
ComfyAudioJob::ComfyAudioJob(std::unique_ptr<companion::ComfyHttpWindows> transport)
    : http(std::move(transport)) {
  if (!http) throw std::invalid_argument("missing transport");
}
ComfyAudioJob::~ComfyAudioJob() = default;
void ComfyAudioJob::cancel() noexcept {
  cancelled.store(true, std::memory_order_release);
  http->abort();
  pollWake.notify_all();
}
ComfyAudioBatch ComfyAudioJob::run(const ComfyAudioJobRequest &request, ComfyJobAdmission admission) {
  jassert(!ran);
  ComfyAudioBatch batch;
  batch.runtime = request.runtime;
  batch.requestUuid = request.requestUuid;
  batch.request = request.request;
  for (std::size_t i = 0; i != 3; ++i)
    batch.attempts[i].owned = {request.runtime, request.attempts[i], request.request, request.seeds[i]};
  if (std::exchange(ran, true) || !admission.current || !admission.context || !validRequest(request)) return batch;
  const auto finish = [&](std::size_t i, Attempt failure) {
    batch.attempts[i].status = failure;
    if (failure == Attempt::cancelled || failure == Attempt::stale) {
      for (auto &attempt : batch.attempts) if (attempt.raw) { attempt.raw.reset(); attempt.status = failure; }
      batch.rawCount = 0;
      batch.status = terminal(failure);
    } else batch.status = batch.rawCount ? Status::partial : terminal(failure);
  };
  const auto authority = [&](ComfyAuthorityBoundary boundary, std::size_t i) {
    const bool current = admission.current(admission.context, boundary, i);
    if (cancelled.load(std::memory_order_acquire)) return Attempt::cancelled;
    if (!current) return Attempt::stale;
    if (GenerationClock::now() >= request.deadline) return Attempt::timeout;
    return Attempt::unrun;
  };
  for (std::size_t i = 0; i != 3; ++i) {
    auto &attempt = batch.attempts[i];
    try {
      companion::ComfyHttpRequest operation;
      operation.operation = companion::ComfyOperation::submit;
      operation.owned = attempt.owned;
      operation.submitJson = submitBody(request, i);
      if (!validSubmitBody(operation.submitJson)) { finish(i, Attempt::invalidResponse); return batch; }
      if (const auto stop = authority(ComfyAuthorityBoundary::submit, i); stop != Attempt::unrun) { finish(i, stop); return batch; }
      bool ambiguous = false;
      {
        auto reply = http->perform(operation, cancelled, request.deadline);
        attempt.httpStatus = reply.httpStatus;
        if (const auto stop = authority(ComfyAuthorityBoundary::submit, i); stop != Attempt::unrun) { finish(i, stop); return batch; }
        if (reply.status == HttpStatus::httpError && reply.httpStatus == 400 && reply.submitRejected) { finish(i, Attempt::rejected); return batch; }
        if (reply.status != HttpStatus::ok || !acceptedSubmit(reply.json, attempt.owned.prompt)) {
          if (!reply.requestMayHaveReachedServer) { finish(i, httpFailure(reply.status)); return batch; }
          if (reply.status == HttpStatus::cancelled || reply.status == HttpStatus::timeout) { finish(i, httpFailure(reply.status)); return batch; }
          ambiguous = true;
          attempt.status = Attempt::submissionAmbiguous;
        }
      }
      operation.submitJson.clear();
      operation.operation = companion::ComfyOperation::history;
      ComfyOwnedOutput output;
      for (;;) {
        if (const auto stop = authority(ComfyAuthorityBoundary::history, i); stop != Attempt::unrun) { finish(i, ambiguous && stop == Attempt::timeout ? Attempt::submissionAmbiguous : stop); return batch; }
        OwnedHistoryStatus result;
        {
          auto reply = http->perform(operation, cancelled, request.deadline);
          attempt.httpStatus = reply.httpStatus;
          if (const auto stop = authority(ComfyAuthorityBoundary::history, i); stop != Attempt::unrun) { finish(i, ambiguous && stop == Attempt::timeout ? Attempt::submissionAmbiguous : stop); return batch; }
          if (reply.status != HttpStatus::ok) { finish(i, httpFailure(reply.status)); return batch; }
          result = history(reply.json, attempt.owned, output);
        }
        if (result == OwnedHistoryStatus::remoteFailed) { finish(i, Attempt::remoteFailed); return batch; }
        if (result == OwnedHistoryStatus::invalid) { finish(i, Attempt::invalidResponse); return batch; }
        if (result == OwnedHistoryStatus::success) break;
        std::unique_lock lock(pollMutex);
        pollWake.wait_until(lock, std::min(request.deadline, GenerationClock::now() + std::chrono::milliseconds(100)), [&] { return cancelled.load(std::memory_order_acquire); });
      }
      operation.operation = companion::ComfyOperation::download;
      operation.filename = std::move(output.filename);
      operation.subfolder = std::move(output.subfolder);
      if (const auto stop = authority(ComfyAuthorityBoundary::download, i); stop != Attempt::unrun) { finish(i, stop); return batch; }
      auto reply = http->perform(operation, cancelled, request.deadline);
      attempt.httpStatus = reply.httpStatus;
      if (const auto stop = authority(ComfyAuthorityBoundary::download, i); stop != Attempt::unrun) { finish(i, stop); return batch; }
      if (reply.status != HttpStatus::ok) { finish(i, httpFailure(reply.status)); return batch; }
      if (!reply.raw || !reply.raw->size() || reply.raw->size() > binaryLimit) { finish(i, Attempt::invalidResponse); return batch; }
      attempt.encodedBytes = reply.raw->size();
      attempt.fileSha256 = reply.raw->sha256();
      attempt.raw = std::move(reply.raw);
      attempt.status = Attempt::rawReady;
      ++batch.rawCount;
    } catch (const std::bad_alloc &) { finish(i, Attempt::allocationFailed); return batch; }
  }
  batch.status = Status::complete;
  return batch;
}
bool buildComfyAudioSubmit(const ComfyAudioJobRequest &request, std::size_t ordinal, std::string &destination) noexcept {
  try {
    if (ordinal >= 3 || !validRequest(request)) return false;
    auto candidate = submitBody(request, ordinal);
    if (!validSubmitBody(candidate)) return false;
    destination = std::move(candidate);
    return true;
  } catch (const std::bad_alloc &) { return false; }
}
OwnedHistoryStatus parseOwnedComfyHistory(std::string_view text, const companion::ComfyOwnedAttempt &owned, ComfyOwnedOutput &destination) noexcept {
  try { return history(text, owned, destination); }
  catch (const std::bad_alloc &) { return OwnedHistoryStatus::invalid; }
}
} // namespace airack::drums
