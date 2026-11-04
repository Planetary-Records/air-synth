#include "ComfyHttpWindows.h"
#include "StrictJson.h"
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <initializer_list>
#include <juce_cryptography/juce_cryptography.h>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <thread>
#include <windows.h>
#include <winhttp.h>
#include <winternl.h>

namespace airack::companion {
namespace {
using Clock = std::chrono::steady_clock;
using Status = ComfyHttpStatus;
using Point = detail::ComfyTestPoint;
using Fault = detail::ComfyTestFault;
constexpr std::size_t chunkSize = 65536, jsonLimit = 262144,
                      binaryLimit = 16777216, headerLimit = 32768;
std::atomic_uint liveStates{0}, liveCallbacks{0};
void point(detail::ComfyTestControl *t, Point p, const juce::File &f) noexcept {
  if (t && t->point)
    t->point(t->context, p, f);
}
bool fault(detail::ComfyTestControl *t, Fault f) noexcept {
  return t && t->fault == f;
}
struct FileHandle {
  HANDLE value = INVALID_HANDLE_VALUE;
  FileHandle() = default;
  explicit FileHandle(HANDLE h) : value(h) {}
  ~FileHandle() {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  FileHandle(FileHandle &&other) noexcept
      : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
  FileHandle &operator=(FileHandle &&other) noexcept {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
    value = std::exchange(other.value, INVALID_HANDLE_VALUE);
    return *this;
  }
  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;
};
bool validOperation(ComfyOperation op) noexcept {
  return op == ComfyOperation::submit || op == ComfyOperation::history ||
         op == ComfyOperation::cancelOwned || op == ComfyOperation::download;
}
bool validDownload(const ComfyHttpRequest &r) {
  if (!r.submitJson.empty())
    return false;
  const auto parent = "airack/" +
                      r.owned.runtime.toDashedString().toStdString() + "/" +
                      std::to_string(r.owned.request) + "/" +
                      r.owned.prompt.toDashedString().toStdString();
  if (r.subfolder != parent)
    return false;
  const auto prefix = "seed-" + std::to_string(r.owned.seed) + "_";
  constexpr std::string_view suffix = ".flac";
  if (!r.filename.starts_with(prefix) || !r.filename.ends_with(suffix) ||
      r.filename.size() < prefix.size() + suffix.size())
    return false;
  const auto digits =
      std::string_view(r.filename)
          .substr(prefix.size(),
                  r.filename.size() - prefix.size() - suffix.size());
  return !digits.empty() && digits.size() <= 20 &&
         std::all_of(digits.begin(), digits.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}
Status validate(const ComfyHttpRequest &r) {
  if (!validOperation(r.operation) || r.owned.runtime.isNull() ||
      r.owned.prompt.isNull() || !r.owned.request)
    return Status::invalidRequest;
  if (r.operation == ComfyOperation::download)
    return validDownload(r) ? Status::ok : Status::invalidRequest;
  if (!r.filename.empty() || !r.subfolder.empty())
    return Status::invalidRequest;
  if (r.operation != ComfyOperation::submit)
    return r.submitJson.empty() ? Status::ok : Status::invalidRequest;
  if (r.submitJson.size() > 16384)
    return Status::tooLarge;
  juce::var json;
  return ai::parseStrictJson(r.submitJson, json, 16384)
             ? Status::ok
             : Status::invalidRequest;
}
std::string encode(std::string_view text) {
  std::string out;
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
      out += char(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
bool submitRejected(std::string_view text) {
  juce::var decoded;
  if (!ai::parseStrictJson(text, decoded, jsonLimit)) return false;
  const auto exact = [](const juce::var &value, std::initializer_list<const char *> keys) {
    const auto *object = value.getDynamicObject();
    if (!object || object->getProperties().size() != int(keys.size())) return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char *key) { return object->hasProperty(key); });
  };
  const auto errorRecord = [&](const juce::var &value) {
    return exact(value, {"type", "message", "details", "extra_info"}) &&
      value["type"].isString() && value["type"].toString().isNotEmpty() &&
      value["message"].isString() && value["details"].isString() && value["extra_info"].getDynamicObject() != nullptr;
  };
  if (!exact(decoded, {"error", "node_errors"}) || !errorRecord(decoded["error"]) || !decoded["node_errors"].getDynamicObject()) return false;
  const auto type = decoded["error"]["type"].toString();
  if (type != "invalid_prompt_id" && type != "no_prompt" && type != "missing_node_type" &&
      type != "prompt_no_outputs" && type != "prompt_outputs_failed_validation") return false;
  const auto &nodes = decoded["node_errors"].getDynamicObject()->getProperties();
  for (int i = 0; i < nodes.size(); ++i) {
    const auto &node = nodes.getValueAt(i);
    if (nodes.getName(i).toString().isEmpty() || !exact(node, {"errors", "dependent_outputs", "class_type"}) ||
        !node["class_type"].isString() || node["class_type"].toString().isEmpty()) return false;
    const auto *errors = node["errors"].getArray(), *outputs = node["dependent_outputs"].getArray();
    if (!errors || errors->isEmpty() || !outputs) return false;
    for (const auto &error : *errors) if (!errorRecord(error)) return false;
    for (const auto &output : *outputs) if (!output.isString() || output.toString().isEmpty()) return false;
  }
  return true;
}
std::string route(const ComfyHttpRequest &r) {
  const auto id = r.owned.prompt.toDashedString().toStdString();
  switch (r.operation) {
  case ComfyOperation::submit:
    return "/prompt";
  case ComfyOperation::history:
    return "/history/" + id;
  case ComfyOperation::cancelOwned:
    return "/api/jobs/" + id + "/cancel";
  case ComfyOperation::download:
    return "/view?filename=" + encode(r.filename) +
           "&subfolder=" + encode(r.subfolder) + "&type=output";
  }
  return {};
}
// Worker ownership lasts through the session's final callback return.
struct Async {
  detail::ComfyTestControl *tests = nullptr;
  void observe(detail::ComfyNetworkTestPoint p, unsigned code = 0) noexcept {
    if (tests && tests->networkPoint) tests->networkPoint(tests->context, p, code);
  }
  std::atomic_uint references{1};
  HINTERNET session = nullptr, connection = nullptr, request = nullptr;
  HANDLE unloadEvent = nullptr;
  bool unloadArmed = false, drainComplete = false;
  bool closeFaultReported = false, waitFaultReported = false;
  unsigned handlesClosed = 0;
  std::atomic<DWORD> drainError{ERROR_SUCCESS};
  std::mutex api, mutex;
  std::condition_variable ready;
  bool completed = false, closed = false;
  DWORD event = 0, error = 0, bytes = 0;
  std::array<std::byte, chunkSize> buffer{};
  std::string body;
  Async() { ++liveStates; }
  ~Async() {
    if (unloadEvent)
      CloseHandle(unloadEvent);
    --liveStates;
  }
  void retain() noexcept { references.fetch_add(1, std::memory_order_relaxed); }
  void release() noexcept {
    if (references.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete this;
  }
  void rememberDrainError(DWORD error) noexcept {
    DWORD expected = ERROR_SUCCESS;
    drainError.compare_exchange_strong(expected, error ? error : ERROR_GEN_FAILURE,
                                       std::memory_order_relaxed);
  }
  bool armUnloadNotification() noexcept {
    if (fault(tests, Fault::createUnloadEvent)) return false;
    unloadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!unloadEvent) return false;
    if (fault(tests, Fault::armUnloadEvent) ||
        !WinHttpSetOption(session, WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT,
                          &unloadEvent, sizeof(unloadEvent))) return false;
    unloadArmed = true;
    return true;
  }
  void closeOwnedHandles() noexcept {
    unsigned closedCount;
    {
      std::lock_guard lock(api);
      const auto close = [this](HINTERNET &slot) noexcept {
        if (auto handle = std::exchange(slot, nullptr)) {
          const bool closedHandle = WinHttpCloseHandle(handle) != FALSE;
          ++handlesClosed;
          if (!closedHandle) rememberDrainError(GetLastError());
          if (fault(tests, Fault::closeReportOnce) && !closeFaultReported) {
            closeFaultReported = true;
            rememberDrainError(ERROR_GEN_FAILURE);
          }
        }
      };
      close(request);
      close(connection);
      close(session);
      closedCount = handlesClosed;
    }
    observe(detail::ComfyNetworkTestPoint::handlesClosed, closedCount);
  }
  bool closeAndDrain() noexcept {
    closeOwnedHandles();
    if (unloadArmed && !drainComplete) {
      observe(detail::ComfyNetworkTestPoint::beforeUnloadWait);
      for (;;) {
        DWORD wait;
        if (fault(tests, Fault::waitReportOnce) && !waitFaultReported) {
          waitFaultReported = true;
          SetLastError(ERROR_GEN_FAILURE);
          wait = WAIT_FAILED;
        } else wait = WaitForSingleObject(unloadEvent, INFINITE);
        if (wait == WAIT_OBJECT_0) {
          drainComplete = true;
          break;
        }
        rememberDrainError(wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE);
        observe(detail::ComfyNetworkTestPoint::beforeUnloadWait, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      observe(detail::ComfyNetworkTestPoint::afterUnloadWait);
    }
    return drainError.load(std::memory_order_relaxed) == ERROR_SUCCESS;
  }
  static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD code,
                                void *info, DWORD size) noexcept {
    if (!context)
      return;
    auto *self = reinterpret_cast<Async *>(context);
    self->retain();
    ++liveCallbacks;
    {
      std::lock_guard lock(self->mutex);
      if (code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
        self->closed = true;
      else if (code == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        self->error = info && size == sizeof(WINHTTP_ASYNC_RESULT)
                          ? static_cast<WINHTTP_ASYNC_RESULT *>(info)->dwError
                          : ERROR_WINHTTP_INTERNAL_ERROR;
        self->completed = true;
      } else if (code == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
                 code == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                 code == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
        self->event = code;
        self->bytes = size;
        self->completed = true;
      }
      self->ready.notify_all();
    }
    self->observe(detail::ComfyNetworkTestPoint::callbackLeaving, code);
    if (code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
      self->release();
    --liveCallbacks;
    self->release();
  }
  template <class Fn> bool issue(Fn &&fn) {
    std::lock_guard call(api);
    if (!request)
      return false;
    {
      std::lock_guard lock(mutex);
      completed = false;
      event = error = bytes = 0;
    }
    if (!fn(request)) {
      const auto failure = GetLastError();
      std::lock_guard lock(mutex);
      error = failure;
      completed = true;
      return false;
    }
    return true;
  }
  Status wait(DWORD expected, const std::atomic_bool &cancelled,
              const std::atomic_bool &aborted, Clock::time_point deadline,
              unsigned seconds) {
    const auto end =
        std::min(deadline, Clock::now() + std::chrono::seconds(seconds));
    for (;;) {
      if (cancelled.load() || aborted.load()) {
        closeOwnedHandles();
        return Status::cancelled;
      }
      if (Clock::now() >= end) {
        closeOwnedHandles();
        return Status::timeout;
      }
      std::unique_lock lock(mutex);
      if (completed) {
        if (error)
          return error == ERROR_WINHTTP_TIMEOUT ? Status::timeout
                                                : Status::unavailable;
        return event == expected ? Status::ok : Status::invalidResponse;
      }
      ready.wait_until(
          lock, std::min(end, Clock::now() + std::chrono::milliseconds(10)));
    }
  }
};
struct AsyncOwner {
  Async *value = new Async;
  explicit AsyncOwner(detail::ComfyTestControl *t) { value->tests = t; }
  AsyncOwner(const AsyncOwner &) = delete;
  AsyncOwner &operator=(const AsyncOwner &) = delete;
  ~AsyncOwner() {
    value->closeAndDrain();
    value->observe(detail::ComfyNetworkTestPoint::beforeOwnerRelease);
    value->release();
  }
};
std::string lower(std::string text) {
  for (auto &c : text)
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
  return text;
}
std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}
struct Headers {
  unsigned status = 0;
  std::optional<std::uint64_t> length;
  bool chunked = false;
  std::string type;
};
Status headers(Async &a, Headers &out) {
  std::lock_guard call(a.api);
  if (!a.request)
    return Status::cancelled;
  DWORD n = sizeof(out.status);
  if (!WinHttpQueryHeaders(
          a.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &out.status, &n,
          WINHTTP_NO_HEADER_INDEX))
    return Status::invalidResponse;
  n = 0;
  WinHttpQueryHeaders(a.request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                      WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &n,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || n > 2 * (headerLimit + 1))
    return Status::tooLarge;
  std::vector<wchar_t> raw((n + 1) / 2);
  if (!WinHttpQueryHeaders(a.request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                           WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &n,
                           WINHTTP_NO_HEADER_INDEX))
    return Status::invalidResponse;
  const auto utf8 = juce::String(raw.data()).toStdString();
  if (utf8.size() > headerLimit)
    return Status::tooLarge;
  bool haveType = false, haveEncoding = false, haveTransfer = false;
  std::string_view rest = utf8;
  const auto first = rest.find("\r\n");
  if (first == rest.npos)
    return Status::invalidResponse;
  rest.remove_prefix(first + 2);
  while (!rest.empty()) {
    const auto end = rest.find("\r\n");
    if (end == rest.npos)
      return Status::invalidResponse;
    const auto line = rest.substr(0, end);
    rest.remove_prefix(end + 2);
    if (line.empty())
      break;
    const auto colon = line.find(':');
    if (colon == line.npos || colon == 0)
      return Status::invalidResponse;
    const auto key = lower(std::string(line.substr(0, colon)));
    const auto value = trim(line.substr(colon + 1));
    if (key == "content-length") {
      if (out.length || value.empty() || value.size() > 20)
        return Status::invalidResponse;
      std::uint64_t length = 0;
      const auto [p, ec] =
          std::from_chars(value.data(), value.data() + value.size(), length);
      if (ec != std::errc{} || p != value.data() + value.size())
        return Status::invalidResponse;
      out.length = length;
    } else if (key == "transfer-encoding") {
      if (haveTransfer || lower(std::string(value)) != "chunked")
        return Status::invalidResponse;
      haveTransfer = out.chunked = true;
    } else if (key == "content-encoding") {
      if (haveEncoding || lower(std::string(value)) != "identity")
        return Status::invalidResponse;
      haveEncoding = true;
    } else if (key == "content-type") {
      if (haveType)
        return Status::invalidResponse;
      haveType = true;
      out.type = lower(std::string(trim(value.substr(0, value.find(';')))));
    }
  }
  if (out.length && out.chunked)
    return Status::invalidResponse;
  return Status::ok;
}
// Private native calls preserve RootDirectory identity; there is no pathname
// fallback.
struct NativeFiles {
  using Set = NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
  decltype(&NtCreateFile) create = nullptr;
  Set set = nullptr;
  NativeFiles() {
    if (const auto module = GetModuleHandleW(L"ntdll.dll")) {
      create = reinterpret_cast<decltype(create)>(
          GetProcAddress(module, "NtCreateFile"));
      set =
          reinterpret_cast<Set>(GetProcAddress(module, "NtSetInformationFile"));
    }
  }
  explicit operator bool() const noexcept { return create && set; }
};
// Synchronous nonalert file handles normally complete inline. Unexpected
// pending I/O still owns every input until terminal completion, even on an
// abnormal filesystem.
NTSTATUS drain(NTSTATUS status, IO_STATUS_BLOCK &io, HANDLE &file,
               const std::atomic_bool &cancel, const std::atomic_bool &abort,
               Clock::time_point deadline) noexcept {
  if (status != NTSTATUS(0x103))
    return status;
  bool requested = false;
  while (
      InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&io.Status),
                                 0x103, 0x103) == 0x103) {
    if (!requested && file && file != INVALID_HANDLE_VALUE &&
        (cancel.load() || abort.load() || Clock::now() >= deadline)) {
      CancelIoEx(file, nullptr);
      requested = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return io.Status;
}
static_assert(sizeof(void *) == 8 && sizeof(FILE_RENAME_INFO) == 24 &&
              alignof(FILE_RENAME_INFO) == 8);
static_assert(offsetof(FILE_RENAME_INFO, RootDirectory) == 8 &&
              offsetof(FILE_RENAME_INFO, FileNameLength) == 16 &&
              offsetof(FILE_RENAME_INFO, FileName) == 20);
bool fileIdentity(HANDLE file, FILE_ID_INFO &id) noexcept {
  return GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) != 0;
}
bool sameIdentity(const FILE_ID_INFO &a, const FILE_ID_INFO &b) noexcept {
  return a.VolumeSerialNumber == b.VolumeSerialNumber &&
         std::memcmp(a.FileId.Identifier, b.FileId.Identifier, 16) == 0;
}
} // namespace
struct ComfyRawFile::Identity {
  FileHandle file;
  std::vector<FileHandle> directories;
  FILE_ID_INFO id{};
  std::array<std::byte, chunkSize> scratch{};
  detail::ComfyTestControl *tests = nullptr;
  bool valid() const noexcept {
    FILE_ID_INFO now{};
    return file.value != INVALID_HANDLE_VALUE &&
           !fault(tests, Fault::identity) && fileIdentity(file.value, now) &&
           sameIdentity(id, now);
  }
};
struct ComfyHttpWindows::Impl {
  ComfyLoopback loopback;
  juce::File root;
  std::uint16_t port;
  std::atomic_bool aborted{false};
  std::mutex mutex;
  Async *active = nullptr;
  detail::ComfyTestControl *tests = nullptr;
  Impl(ComfyLoopback v, juce::File f, std::uint16_t p)
      : loopback(v), root(std::move(f)), port(p) {}
};
ComfyHttpWindows::ComfyHttpWindows(ComfyLoopback v, juce::File f)
    : ComfyHttpWindows(v, std::move(f), 8189) {}
ComfyHttpWindows::ComfyHttpWindows(ComfyLoopback v, juce::File f,
                                   std::uint16_t p)
    : impl(std::make_unique<Impl>(v, std::move(f), p)) {}
ComfyHttpWindows::~ComfyHttpWindows() { abort(); }
void ComfyHttpWindows::abort() noexcept {
  impl->aborted.store(true);
  Async *active = nullptr;
  {
    std::lock_guard lock(impl->mutex);
    active = impl->active;
    if (active)
      active->retain();
  }
  if (active) {
    active->closeOwnedHandles();
    active->release();
  }
}
void ComfyHttpWindows::setTestControl(detail::ComfyTestControl *t) noexcept {
  impl->tests = t;
}
std::array<unsigned, 2> ComfyHttpWindows::liveNetworkState() noexcept {
  return {liveStates.load(), liveCallbacks.load()};
}
ComfyRawFile::~ComfyRawFile() {
  if (!identity)
    return;
  auto &i = *identity;
  point(i.tests, Point::beforeDisposition, path);
  if (i.valid()) {
    if (i.tests)
      ++i.tests->dispositionCalls;
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!fault(i.tests, Fault::disposition))
      SetFileInformationByHandle(i.file.value, FileDispositionInfo,
                                 &disposition, sizeof(disposition));
  }
  if (i.file.value != INVALID_HANDLE_VALUE)
    CloseHandle(std::exchange(i.file.value, INVALID_HANDLE_VALUE));
  point(i.tests, Point::afterClose, path);
}
bool ComfyRawFile::readAt(std::uint64_t offset,
                          std::span<std::byte> destination) const noexcept {
  if (!identity || destination.empty() || destination.size() > chunkSize ||
      offset > byteCount || destination.size() > byteCount - offset ||
      !identity->valid() || fault(identity->tests, Fault::read))
    return false;
  LARGE_INTEGER position{};
  position.QuadPart = LONGLONG(offset);
  if (!SetFilePointerEx(identity->file.value, position, nullptr, FILE_BEGIN))
    return false;
  DWORD read = 0;
  const auto requested = DWORD(destination.size());
  const auto actual =
      fault(identity->tests, Fault::shortRead) ? requested - 1 : requested;
  if (!ReadFile(identity->file.value, identity->scratch.data(), actual, &read,
                nullptr) ||
      read != requested)
    return false;
  std::memcpy(destination.data(), identity->scratch.data(), destination.size());
  return true;
}
ComfyHttpReply ComfyHttpWindows::perform(const ComfyHttpRequest &request,
                                         const std::atomic_bool &cancelled,
                                         Clock::time_point deadline) {
  std::optional<AsyncOwner> network;
  bool submitRejectedCandidate = false;
  const auto stopped = [&]() {
    if (cancelled.load() || impl->aborted.load())
      return Status::cancelled;
    if (Clock::now() >= deadline)
      return Status::timeout;
    return Status::ok;
  };
  const auto run = [&]() -> ComfyHttpReply {
  ComfyHttpReply result;
  result.status = stopped();
  if (result.status != Status::ok)
    return result;
  if (impl->loopback != ComfyLoopback::ipv4 &&
      impl->loopback != ComfyLoopback::ipv6) {
    result.status = Status::invalidRequest;
    return result;
  }
  try {
    result.status = validate(request);
    if (result.status != Status::ok)
      return result;
    std::unique_ptr<ComfyRawFile> raw;
    if (request.operation == ComfyOperation::download) {
      raw = std::unique_ptr<ComfyRawFile>(new ComfyRawFile);
      raw->identity = std::make_unique<ComfyRawFile::Identity>();
      auto &id = *raw->identity;
      id.tests = impl->tests;
      const std::wstring root(impl->root.getFullPathName().toWideCharPointer());
      if (root.size() < 3 || root[1] != L':' ||
          (root[2] != L'\\' && root[2] != L'/') || root.starts_with(L"\\\\")) {
        result.status = Status::storageFailure;
        return result;
      }
      const auto drive = GetDriveTypeW(root.substr(0, 3).c_str());
      if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE &&
          drive != DRIVE_RAMDISK) {
        result.status = Status::storageFailure;
        return result;
      }
      for (std::size_t at = 3; at <= root.size(); ++at) {
        if (at < root.size() && root[at] != L'\\' && root[at] != L'/')
          continue;
        const auto part = root.substr(0, at);
        FileHandle directory(CreateFileW(
            part.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (directory.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(directory.value, FileAttributeTagInfo,
                                          &attributes, sizeof(attributes)) ||
            !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
          result.status = Status::storageFailure;
          return result;
        }
        id.directories.push_back(std::move(directory));
      }
      if (id.directories.empty()) {
        result.status = Status::storageFailure;
        return result;
      }
      FILE_ID_INFO guarded{}, successorId{};
      FILE_ATTRIBUTE_TAG_INFO attributes{};
      FileHandle successor(CreateFileW(
          root.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (successor.value == INVALID_HANDLE_VALUE ||
          !fileIdentity(id.directories.back().value, guarded) ||
          !fileIdentity(successor.value, successorId) ||
          !sameIdentity(guarded, successorId) ||
          !GetFileInformationByHandleEx(successor.value, FileAttributeTagInfo,
                                        &attributes, sizeof(attributes)) ||
          (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
          !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.status = Status::storageFailure;
        return result;
      }
      id.directories.back() =
          std::move(successor); // Ancestor guards retain deny-write/delete.
      point(impl->tests, Point::rootTransition, impl->root);
      NativeFiles native;
      if (!native) {
        result.status = Status::storageFailure;
        return result;
      }
      const auto name = juce::Uuid().toString() + ".part";
      raw->path = impl->root.getChildFile(name);
      const std::wstring basename(name.toWideCharPointer());
      UNICODE_STRING unicode{};
      unicode.Buffer = const_cast<wchar_t *>(basename.c_str());
      unicode.Length = USHORT(basename.size() * sizeof(wchar_t));
      unicode.MaximumLength = unicode.Length;
      OBJECT_ATTRIBUTES object{};
      object.Length = sizeof(object);
      object.RootDirectory = id.directories.back().value;
      object.ObjectName = &unicode;
      object.Attributes = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE;
      HANDLE created = nullptr;
      IO_STATUS_BLOCK io{};
      io.Status = NTSTATUS(0x103);
      NTSTATUS status = NTSTATUS(0xc0000001);
      if (!fault(impl->tests, Fault::createFile))
        status = native.create(
            &created, GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE,
            &object, &io, nullptr, FILE_ATTRIBUTE_TEMPORARY, FILE_SHARE_READ,
            FILE_CREATE,
            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                FILE_OPEN_REPARSE_POINT,
            nullptr, 0);
      status = drain(status, io, created, cancelled, impl->aborted, deadline);
      if (created && created != INVALID_HANDLE_VALUE)
        id.file = FileHandle(created);
      if (status != 0 || io.Status != 0 || io.Information != FILE_CREATED ||
          id.file.value == INVALID_HANDLE_VALUE ||
          !fileIdentity(id.file.value, id.id)) {
        result.status = stopped();
        if (result.status == Status::ok)
          result.status = Status::storageFailure;
        return result;
      }
    }
    network.emplace(impl->tests);
    auto &state = *network->value;
    state.body = request.submitJson;
    state.session = WinHttpOpen(
        L"AI Rack Synth Local Audio", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!state.session) {
      result.status = Status::unavailable;
      return result;
    }
    if (!state.armUnloadNotification()) {
      result.status = Status::unavailable;
      return result;
    }
    if (impl->tests && impl->tests->sessionObserved)
      impl->tests->sessionObserved(impl->tests->context, state.session, state.unloadEvent, reinterpret_cast<const void *>(&Async::callback));
    BOOL noPooling = TRUE;
    WINHTTP_FAILED_CONNECTION_RETRIES retries{};
    if (fault(impl->tests, Fault::setupAfterArm) || !WinHttpSetTimeouts(state.session, 10000, 10000, 10000, 15000) ||
        !WinHttpSetOption(state.session, WINHTTP_OPTION_DISABLE_GLOBAL_POOLING,
                          &noPooling, sizeof(noPooling)) ||
        !WinHttpSetOption(state.session,
                          WINHTTP_OPTION_FAILED_CONNECTION_RETRIES, &retries,
                          sizeof(retries))) {
      result.status = Status::unavailable;
      return result;
    }
    state.connection = WinHttpConnect(
        state.session,
        impl->loopback == ComfyLoopback::ipv4 ? L"127.0.0.1" : L"[::1]",
        impl->port, 0);
    const auto routeText = juce::String::fromUTF8(route(request).c_str());
    const auto path = routeText.toWideCharPointer();
    const bool post = request.operation == ComfyOperation::submit ||
                      request.operation == ComfyOperation::cancelOwned;
    state.request =
        state.connection
            ? WinHttpOpenRequest(state.connection, post ? L"POST" : L"GET",
                                 path, L"HTTP/1.1", WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
            : nullptr;
    if (!state.request) {
      result.status = Status::unavailable;
      return result;
    }
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES |
                     WINHTTP_DISABLE_AUTHENTICATION |
                     WINHTTP_DISABLE_KEEP_ALIVE;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH,
          maxHeaders = DWORD(headerLimit);
    if (!WinHttpSetOption(state.request, WINHTTP_OPTION_DISABLE_FEATURE,
                          &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(state.request, WINHTTP_OPTION_AUTOLOGON_POLICY,
                          &autologon, sizeof(autologon)) ||
        !WinHttpSetOption(state.request,
                          WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &maxHeaders,
                          sizeof(maxHeaders))) {
      state.closeOwnedHandles();
      result.status = Status::unavailable;
      return result;
    }
    if (WinHttpSetStatusCallback(state.request, Async::callback,
                                 WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                                     WINHTTP_CALLBACK_FLAG_HANDLES,
                                 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
      state.closeOwnedHandles();
      result.status = Status::unavailable;
      return result;
    }
    state.retain();
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&state);
    if (!WinHttpSetOption(state.request, WINHTTP_OPTION_CONTEXT_VALUE, &context,
                          sizeof(context))) {
      state.release();
      state.closeOwnedHandles();
      result.status = Status::unavailable;
      return result;
    }
    {
      std::lock_guard lock(impl->mutex);
      state.retain();
      impl->active = &state;
    }
    struct Cleanup {
      Impl &impl;
      Async &state;
      ~Cleanup() {
        {
          std::lock_guard lock(impl.mutex);
          impl.active = nullptr;
        }
        state.release();
      }
    } cleanup{*impl, state};
    point(impl->tests, Point::beforeSend, impl->root);
    result.status = stopped();
    if (result.status != Status::ok)
      return result;
    result.requestMayHaveReachedServer = true;
    const auto sendHeaders =
        request.operation == ComfyOperation::submit
            ? L"Content-Type: application/json\r\nAccept-Encoding: "
              L"identity\r\nConnection: close\r\n"
            : L"Accept-Encoding: identity\r\nConnection: close\r\n";
    const auto sent = state.issue([&](HINTERNET h) {
      return WinHttpSendRequest(
          h, sendHeaders, DWORD(-1),
          state.body.empty() ? WINHTTP_NO_REQUEST_DATA : state.body.data(),
          DWORD(state.body.size()), DWORD(state.body.size()), context);
    });
    point(impl->tests, Point::sendPending, impl->root);
    result.status = state.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,
                               cancelled, impl->aborted, deadline, 10);
    if (!sent || result.status != Status::ok)
      return result;
    const auto receiving = state.issue(
        [](HINTERNET h) { return WinHttpReceiveResponse(h, nullptr); });
    result.status = state.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
                               cancelled, impl->aborted, deadline, 10);
    if (!receiving || result.status != Status::ok)
      return result;
    Headers parsed;
    result.status = headers(state, parsed);
    result.httpStatus = parsed.status;
    if (result.status != Status::ok)
      return result;
    point(impl->tests, Point::headers, impl->root);
    const bool success = parsed.status >= 200 && parsed.status < 300;
    const bool rejectBody = request.operation == ComfyOperation::submit &&
                            parsed.status == 400 && parsed.type == "application/json";
    const auto cap = success && raw ? binaryLimit : jsonLimit;
    if (parsed.length && *parsed.length > cap) {
      result.status = Status::tooLarge;
      return result;
    }
    if (success &&
        (raw ? (parsed.type != "audio/flac" && parsed.type != "audio/x-flac" &&
                parsed.type != "application/octet-stream")
             : parsed.type != "application/json")) {
      result.status = Status::invalidResponse;
      return result;
    }
    std::uint64_t total = 0;
    std::string json;
    std::array<std::byte, 4> signature{};
    for (;;) {
      const auto reading = state.issue([&](HINTERNET h) {
        return WinHttpReadData(
            h, state.buffer.data(),
            success && raw && total < 4 ? 1u : DWORD(state.buffer.size()),
            nullptr);
      });
      result.status = state.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE,
                                 cancelled, impl->aborted, deadline, 15);
      if (!reading || result.status != Status::ok)
        return result;
      const auto count = state.bytes;
      if (count > cap - total) {
        result.status = Status::tooLarge;
        return result;
      }
      if (!count)
        break;
      if (impl->tests && !impl->tests->firstBodyChunk)
        impl->tests->firstBodyChunk = count;
      if (success && raw) {
        if (total < 4) {
          const auto first = std::min<std::uint64_t>(count, 4 - total);
          std::memcpy(signature.data() + total, state.buffer.data(),
                      std::size_t(first));
          if (total + count >= 4 &&
              std::memcmp(signature.data(), "fLaC", 4) != 0) {
            result.status = Status::invalidResponse;
            return result;
          }
        }
        DWORD written = 0;
        if (fault(impl->tests, Fault::write) ||
            !WriteFile(raw->identity->file.value, state.buffer.data(), count,
                       &written, nullptr) ||
            written != count) {
          result.status = Status::storageFailure;
          return result;
        }
      } else if (success || rejectBody)
        json.append(reinterpret_cast<const char *>(state.buffer.data()), count);
      total += count;
      point(impl->tests, Point::chunk, raw ? raw->path : impl->root);
      result.status = stopped();
      if (result.status != Status::ok)
        return result;
    }
    if (parsed.length && total != *parsed.length) {
      result.status = Status::invalidResponse;
      return result;
    }
    if (!success) {
      if (rejectBody) submitRejectedCandidate = submitRejected(json);
      result.status = Status::httpError;
      return result;
    }
    if (raw) {
      if (total < 4) {
        result.status = Status::invalidResponse;
        return result;
      }
      raw->byteCount = total;
      class HeldStream final : public juce::InputStream {
      public:
        HeldStream(ComfyRawFile &f, decltype(stopped) &check)
            : file(f), check(check) {}
        juce::int64 getTotalLength() override {
          return juce::int64(file.size());
        }
        juce::int64 getPosition() override { return juce::int64(position); }
        bool setPosition(juce::int64) override { return false; }
        bool isExhausted() override { return position == file.size() || !good; }
        int read(void *out, int n) override {
          if (check() != Status::ok) {
            good = false;
            return 0;
          }
          const auto count = std::size_t(
              std::min<std::uint64_t>({std::uint64_t(std::max(0, n)),
                                       file.size() - position, chunkSize}));
          if (!count)
            return 0;
          if (!file.readAt(position, {static_cast<std::byte *>(out), count})) {
            good = false;
            return 0;
          }
          position += count;
          return int(count);
        }
        bool good = true;
        std::uint64_t position = 0;
        ComfyRawFile &file;
        decltype(stopped) &check;
      } input(*raw, stopped);
      const auto digest = juce::SHA256(input).getRawData();
      if (!input.good || input.position != total) {
        result.status = stopped();
        if (result.status == Status::ok)
          result.status = Status::storageFailure;
        return result;
      }
      std::memcpy(raw->digest.data(), digest.getData(), 32);
      if (fault(impl->tests, Fault::flush) ||
          !FlushFileBuffers(raw->identity->file.value)) {
        result.status = Status::storageFailure;
        return result;
      }
      result.status = stopped();
      if (result.status != Status::ok)
        return result;
      point(impl->tests, Point::beforeRename, raw->path);
      result.status = stopped();
      if (result.status != Status::ok)
        return result;
      const auto name = impl->tests && impl->tests->finalBasename
                            ? juce::String(impl->tests->finalBasename)
                            : juce::Uuid().toString() + ".flac";
      const auto wide = name.toWideCharPointer();
      const auto chars = std::wcslen(wide);
      std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) +
                                     (chars + 1) * sizeof(wchar_t));
      auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
      rename->ReplaceIfExists = FALSE;
      rename->RootDirectory = raw->identity->directories.back().value;
      rename->FileNameLength = DWORD(chars * sizeof(wchar_t));
      std::memcpy(rename->FileName, wide, rename->FileNameLength);
      if (impl->tests)
        ++impl->tests->renameCalls;
      NativeFiles native;
      IO_STATUS_BLOCK io{};
      io.Status = NTSTATUS(0x103);
      NTSTATUS status = NTSTATUS(0xc0000001);
      if (native && !fault(impl->tests, Fault::rename))
        status = native.set(raw->identity->file.value, &io, rename,
                            ULONG(storage.size()), 10);
      status = drain(status, io, raw->identity->file.value, cancelled,
                     impl->aborted, deadline);
      if (status != 0 || io.Status != 0) {
        if (impl->tests)
          impl->tests->osError = ULONG(status);
        result.status = stopped();
        if (result.status == Status::ok)
          result.status = Status::storageFailure;
        return result;
      }

      raw->path = impl->root.getChildFile(name);
      point(impl->tests, Point::afterRename, raw->path);
      result.status = stopped();
      if (result.status != Status::ok)
        return result;
      result.raw = std::move(raw);
    } else {
      juce::var decoded;
      if (!ai::parseStrictJson(json, decoded, jsonLimit)) {
        result.status = Status::invalidResponse;
        return result;
      }
      result.json = std::move(json);
    }
    result.status = stopped();
    if (result.status != Status::ok) {
      result.raw.reset();
      result.json.clear();
    }
    return result;
  } catch (const std::bad_alloc &) {
    result.status = Status::storageFailure;
    result.raw.reset();
    result.json.clear();
    return result;
  }
  };
  auto result = run();
  if (network)
    network->value->observe(detail::ComfyNetworkTestPoint::resultSelected,
                            unsigned(result.status));
  const bool cleanupHealthy = !network || network->value->closeAndDrain();
  const bool rejectedCandidate = submitRejectedCandidate &&
      result.status == Status::httpError && result.httpStatus == 400;
  if (result.status == Status::ok || rejectedCandidate) {
    const auto eligibility = stopped();
    if (eligibility != Status::ok)
      result.status = eligibility;
    else if (!cleanupHealthy)
      result.status = Status::unavailable;
    else if (rejectedCandidate)
      result.submitRejected = true;
  }
  if (result.status != Status::ok) {
    result.raw.reset();
    result.json.clear();
  }
  return result;
}
} // namespace airack::companion
