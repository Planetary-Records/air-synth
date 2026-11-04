#include "ComfyHttpUnloadTestApi.h"
#include "ComfyHttpWindows.h"
#include <array>
#include <cstring>
#include <cstdio>
#include <thread>
#include <juce_cryptography/juce_cryptography.h>
#include <winhttp.h>

namespace airack::companion::detail {
struct ComfyHttpTestAccess {
  static void control(ComfyHttpWindows &h, ComfyTestControl *c) {
    h.setTestControl(c);
  }
  static std::array<unsigned, 2> live() {
    return ComfyHttpWindows::liveNetworkState();
  }
  static std::unique_ptr<ComfyHttpWindows>
  fixture(ComfyLoopback kind, juce::File root, std::uint16_t port) {
    return std::unique_ptr<ComfyHttpWindows>(
        new ComfyHttpWindows(kind, std::move(root), port));
  }
};
}
using namespace airack::companion;
namespace {
using Clock = std::chrono::steady_clock;
using Case = ComfyUnloadCase;
bool download(Case c) {
  return c == Case::downloadSuccess || c == Case::downloadCancelDuringDrain ||
         c == Case::downloadAbortDuringDrain ||
         c == Case::downloadDeadlineDuringDrain;
}
struct Context {
  const ComfyUnloadInput &input;
  ComfyUnloadWitness &witness;
  bool setupFailed = false;
  bool retirementMismatch = false;
  juce::File rawPath;
  Clock::time_point waitFailure{};
  DWORD workerThread = GetCurrentThreadId(), closingThread = 0;
  bool waitErrorAfterSignal = false;
};
void observed(void *p, void *session, void *event,
              const void *callback) noexcept {
  auto &c = *static_cast<Context *>(p);
  c.witness.callbackAddress = reinterpret_cast<std::uintptr_t>(callback);
  if (event) {
    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                         &c.witness.sessionEvent, SYNCHRONIZE, FALSE, 0))
      c.setupFailed = true;
  } else {
    c.witness.sessionEvent = c.input.baselineSessionEvent;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT,
                          &c.witness.sessionEvent, sizeof(HANDLE)))
      c.setupFailed = true;
  }
  SetEvent(c.input.sessionObserved);
}
void networkPoint(void *p, detail::ComfyNetworkTestPoint point,
                  unsigned code) noexcept {
  auto &c = *static_cast<Context *>(p);
  using Point = detail::ComfyNetworkTestPoint;
  if (point == Point::callbackLeaving &&
      code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
    c.closingThread = GetCurrentThreadId();
    SetEvent(c.input.callbackEntered);
    WaitForSingleObject(c.input.callbackRelease, INFINITE);
  } else if (point == Point::handlesClosed) {
    InterlockedExchange(&c.witness.allHandlesClosed, LONG(code));
  } else if (point == Point::beforeUnloadWait && code) {
    InterlockedIncrement(&c.witness.waitErrors);
    c.waitFailure = Clock::now();
    c.waitErrorAfterSignal = WaitForSingleObject(c.witness.sessionEvent, 0) == WAIT_OBJECT_0;
  } else if (point == Point::afterUnloadWait && c.waitFailure != Clock::time_point{}) {
    if (Clock::now() - c.waitFailure < std::chrono::milliseconds(10)) c.setupFailed = true;
  } else if (point == Point::resultSelected && code == unsigned(ComfyHttpStatus::ok)) {
    SetEvent(c.input.completedBody);
  } else if (point == Point::beforeOwnerRelease && c.input.protectRetirement &&
             c.witness.sessionEvent &&
             WaitForSingleObject(c.witness.sessionEvent, 0) != WAIT_OBJECT_0) {
    c.retirementMismatch = true;
    SetEvent(c.input.ownerRetireAttempted);
    // A deliberately broken drain must keep the original event/context alive.
    WaitForSingleObject(c.witness.sessionEvent, INFINITE);
  }
}
}
extern "C" __declspec(dllexport) DWORD WINAPI ComfyHttpUnloadOwnedRun(
    const ComfyUnloadInput *input, ComfyUnloadWitness *witness) noexcept {
  if (!input || !witness || input->size != sizeof(*input) ||
      witness->size != sizeof(*witness) || !input->ownedRoot ||
      !input->sessionObserved || !input->performReturned ||
      !input->callbackEntered || !input->callbackRelease ||
      !input->baselineSessionEvent || !input->ownerRetireAttempted ||
      !input->completedBody || !input->stopApplied || !input->abortStart)
    return 2;
  try {
    Context context{*input, *witness};
    auto http = detail::ComfyHttpTestAccess::fixture(
        ComfyLoopback::ipv4, juce::File(juce::String(input->ownedRoot)), input->port);
    detail::ComfyTestControl control;
    control.context = &context;
    switch (input->scenario) {
    case Case::createEventFailure: control.fault = detail::ComfyTestFault::createUnloadEvent; break;
    case Case::armEventFailure: control.fault = detail::ComfyTestFault::armUnloadEvent; break;
    case Case::closeReportFailure: control.fault = detail::ComfyTestFault::closeReportOnce; break;
    case Case::waitReportFailure: control.fault = detail::ComfyTestFault::waitReportOnce; break;
    case Case::setupAfterArmFailure: control.fault = detail::ComfyTestFault::setupAfterArm; break;
    default: break;
    }
    control.sessionObserved = observed;
    control.networkPoint = networkPoint;
    if (input->scenario == Case::cancelPending || input->scenario == Case::timeoutPending ||
        input->scenario >= Case::historyCancelDuringDrain)
      control.fault = detail::ComfyTestFault::closeReportOnce;
    control.point = [](void *p, detail::ComfyTestPoint point,
                        const juce::File &file) noexcept {
      if (point == detail::ComfyTestPoint::afterRename)
        static_cast<Context *>(p)->rawPath = file;
    };
    detail::ComfyHttpTestAccess::control(*http, &control);
    ComfyHttpRequest request;
    request.operation = download(input->scenario) ? ComfyOperation::download
                                                 : ComfyOperation::history;
    request.owned.runtime = juce::Uuid("00112233-4455-6677-8899-aabbccddeeff");
    request.owned.prompt = juce::Uuid("10213243-5465-7687-98a9-bacbdcedfe0f");
    request.owned.request = 1;
    request.owned.seed = 17;
    if (download(input->scenario)) {
      request.filename = "seed-17_00001.flac";
      request.subfolder = "airack/00112233-4455-6677-8899-aabbccddeeff/1/"
                          "10213243-5465-7687-98a9-bacbdcedfe0f";
    }
    if (input->scenario == Case::invalidRequest)
      request.owned.request = 0;
    std::atomic_bool cancelled{input->scenario == Case::preCancelled};
    const auto deadline = Clock::now() +
        (input->scenario == Case::expired ? std::chrono::milliseconds(-1)
         : input->scenario == Case::timeoutPending ? std::chrono::milliseconds(250)
                                                  : std::chrono::milliseconds(2000));
    const bool trigger = input->scenario == Case::cancelPending ||
        input->scenario == Case::abortRace ||
        input->scenario >= Case::historyCancelDuringDrain;
    std::array<std::thread, 2> triggers;
    struct JoinTriggers {
      const ComfyUnloadInput &input;
      std::array<std::thread, 2> &threads;
      ~JoinTriggers() {
        SetEvent(input.abortStart);
        for (auto &thread : threads) if (thread.joinable()) thread.join();
      }
    } joinTriggers{*input, triggers};
    if (trigger) {
      triggers[0] = std::thread([&] {
        WaitForSingleObject(input->abortStart, INFINITE);
        if (input->scenario == Case::historyDeadlineDuringDrain ||
            input->scenario == Case::downloadDeadlineDuringDrain)
          std::this_thread::sleep_until(deadline);
        else if (input->scenario == Case::abortRace ||
                 input->scenario == Case::historyAbortDuringDrain ||
                 input->scenario == Case::downloadAbortDuringDrain)
          for (unsigned i = 0; i < 8; ++i) http->abort();
        else cancelled.store(true);
        SetEvent(input->stopApplied);
      });
      if (input->scenario == Case::abortRace)
        triggers[1] = std::thread([&] {
          WaitForSingleObject(input->abortStart, INFINITE);
          for (unsigned i = 0; i < 8; ++i) http->abort();
        });
    }
    auto reply = http->perform(request, cancelled, deadline);
    const bool proof = !witness->sessionEvent ||
        WaitForSingleObject(witness->sessionEvent, 0) == WAIT_OBJECT_0;
    InterlockedExchange(&witness->proofAtPerformReturn, proof ? 1 : 0);
    witness->status = unsigned(reply.status);
    witness->mayHaveReachedServer = reply.requestMayHaveReachedServer;
    witness->rawSize = reply.raw ? unsigned(reply.raw->size()) : 0;
    SetEvent(input->performReturned); // Snapshot precedes every safe rescue.
    if (!proof) WaitForSingleObject(witness->sessionEvent, INFINITE);
    bool contents = true;
    if (reply.status == ComfyHttpStatus::ok) {
      if (download(input->scenario)) {
        std::array<std::byte, 8> bytes{};
        contents = reply.raw && reply.raw->readAt(0, bytes) &&
                   std::memcmp(bytes.data(), "fLaCxxxx", 8) == 0;
        const auto hash = juce::SHA256("fLaCxxxx", 8).getRawData();
        contents = contents && reply.raw->size() == 8 &&
            std::memcmp(reply.raw->sha256().data(), hash.getData(), 32) == 0;
      } else contents = reply.json == "{\"ok\":17}" && !reply.raw;
    } else contents = reply.json.empty() && !reply.raw;
    if (input->scenario >= Case::historyCancelDuringDrain ||
        input->scenario == Case::historySuccess || input->scenario == Case::downloadSuccess ||
        input->scenario == Case::closeReportFailure || input->scenario == Case::waitReportFailure)
      contents = contents && reply.httpStatus == 200 && reply.requestMayHaveReachedServer;
    reply.raw.reset();
    contents = contents && (context.rawPath == juce::File{} || !context.rawPath.exists());
    for (auto &thread : triggers) if (thread.joinable()) thread.join();
    http.reset();
    std::printf("dll_case=%u worker_thread=%lu closing_thread=%lu wait_error_after_signal=%u\n",
        unsigned(input->scenario), context.workerThread, context.closingThread, unsigned(context.waitErrorAfterSignal));
    if (context.setupFailed) return 2;
    return proof && contents && !context.retirementMismatch ? 0 : 1;
  } catch (...) {
    return 2;
  }
}
