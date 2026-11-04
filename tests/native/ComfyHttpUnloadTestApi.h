#pragma once
#include <cstdint>
#include <windows.h>
enum class ComfyUnloadCase : std::uint32_t {
  historySuccess, downloadSuccess, invalidRequest, preCancelled, expired,
  cancelPending, timeoutPending, abortRace, createEventFailure, armEventFailure,
  closeReportFailure, waitReportFailure, setupAfterArmFailure,
  historyCancelDuringDrain, downloadCancelDuringDrain,
  historyAbortDuringDrain, downloadAbortDuringDrain,
  historyDeadlineDuringDrain, downloadDeadlineDuringDrain
};
struct ComfyUnloadInput {
  std::uint32_t size;
  ComfyUnloadCase scenario;
  std::uint16_t port;
  const wchar_t *ownedRoot;
  HANDLE callbackEntered, callbackRelease, abortStart;
  HANDLE sessionObserved, performReturned, baselineSessionEvent;
  HANDLE ownerRetireAttempted;
  HANDLE completedBody, stopApplied;
  BOOL protectRetirement;
};
struct ComfyUnloadWitness {
  std::uint32_t size;
  HANDLE sessionEvent;
  std::uintptr_t callbackAddress;
  volatile LONG proofAtPerformReturn, allHandlesClosed, waitErrors;
  std::uint32_t status, mayHaveReachedServer, rawSize;
};
using ComfyUnloadRun = DWORD (WINAPI *)(const ComfyUnloadInput *,
                                      ComfyUnloadWitness *) noexcept;
