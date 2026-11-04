#include "ComfyHttpFixture.h"
#include "ComfyHttpWindows.h"
#include "ComfyHttpUnloadTestApi.h"
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <juce_cryptography/juce_cryptography.h>
#include <limits>
#include <set>
#include <vector>
#include <winioctl.h>
#include <winternl.h>
#include <winhttp.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

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
} // namespace airack::companion::detail
using namespace airack::companion;
using namespace comfy_fixture;
namespace {
std::atomic_int checks{0}, failures{0};
const char *section = "setup";
bool check(bool pass, const char *expression, int line) {
  ++checks;
  if (!pass) {
    ++failures;
    std::cerr << section << ':' << line << ' ' << expression << '\n';
  }
  return pass;
}
#define CHECK(x) check(bool(x), #x, __LINE__)
bool setupControls() {
  const auto baseline = failures.load();
  section = "fixture_setup_positive";
  for (bool post : {true, false}) {
    const std::string body = post ? "{\"control\":17}" : "";
    const std::string responseBody =
        post ? std::string("fLaC\0\1\0\2", 8) : "{\"label\":\"caf\xc3\xa9\"}";
    const auto method = post ? "POST" : "GET";
    const std::string target = post ? "/fixture-post" : "/fixture-get";
    const std::string request =
        std::string(method) + " " + target +
        " HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
        "Content-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    // Oracle framing is literal and independent of the fixture response
    // builder.
    const std::string expected =
        std::string(
            post
                ? "HTTP/1.1 200 OK\r\nContent-Type: "
                  "audio/flac\r\nContent-Length: 8\r\nConnection: close\r\n\r\n"
                : "HTTP/1.1 200 OK\r\nContent-Type: "
                  "application/json\r\nContent-Length: 17\r\nConnection: "
                  "close\r\n\r\n") +
        responseBody;
    const auto started = Clock::now();
    NativeComfyFixture fixture(
        method, target, body,
        response(responseBody, post ? "audio/flac" : "application/json"));
    CHECK(fixture.valid());
    std::string actual;
    if (fixture.valid())
      CHECK(independentExchange(fixture.port(), request, actual));
    fixture.join();
    CHECK(actual ==
          expected); // status, headers, binary/UTF8 body and exact EOF.
    const auto capture = fixture.captured();
    CHECK(capture.raw == request);
    CHECK(capture.complete && capture.matched);
    CHECK(capture.method == method && capture.target == target &&
          capture.body == body);
    CHECK(fixture.requestCount() == 1 && fixture.exited());
    CHECK(Clock::now() - started < std::chrono::seconds(2));
  }
  section = "fixture_setup_expected_mismatch";
  NativeComfyFixture mismatch("POST", "/expected", "expected", response("ok"));
  const std::string wrong =
      "POST /wrong HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: "
      "5\r\nConnection: close\r\n\r\nwrong";
  std::string actual;
  CHECK(mismatch.valid());
  if (mismatch.valid())
    CHECK(independentExchange(mismatch.port(), wrong, actual));
  mismatch.join();
  const auto capture = mismatch.captured();
  CHECK(capture.complete && !capture.matched && capture.target == "/wrong" &&
        capture.body == "wrong");
  CHECK(actual ==
        "HTTP/1.1 400 Bad Request\r\nContent-Type: "
        "text/plain\r\nContent-Length: 8\r\nConnection: close\r\n\r\nmismatch");
  CHECK(mismatch.requestCount() == 1 && mismatch.exited());
  const bool pass = failures == baseline;
  std::cout << "fixture_setup_positive=" << (pass ? "PASS" : "FAIL")
            << " fixture_setup_mismatch=" << (pass ? "PASS" : "FAIL") << '\n';
  return pass;
}
struct RequestRoot {
  juce::File directory;
  bool valid = false;
  RequestRoot() {
    const auto *env = std::getenv("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT");
    if (!env || !juce::File::isAbsolutePath(juce::String::fromUTF8(env)))
      return;
    const juce::File parent(juce::String::fromUTF8(env));
    directory = parent.getChildFile("comfy-http-" + juce::Uuid().toString());
    if (!directory.isAChildOf(parent) || directory.exists())
      return;
    valid = directory.createDirectory().wasOk();
    const auto attributes =
        GetFileAttributesW(directory.getFullPathName().toWideCharPointer());
    valid = valid && attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
  }
  ~RequestRoot() {
    // Only remove this unique empty fixture directory. Never recursive cleanup.
    if (valid)
      RemoveDirectoryW(directory.getFullPathName().toWideCharPointer());
  }
};
ComfyHttpRequest owned(ComfyOperation operation) {
  ComfyHttpRequest request;
  request.operation = operation;
  request.owned.runtime = juce::Uuid("00112233445566778899aabbccddeeff");
  request.owned.prompt = juce::Uuid("102132435465768798a9bacbdcedfe0f");
  request.owned.request = 1;
  request.owned.seed = 17;
  if (operation == ComfyOperation::download) {
    request.filename = "seed-17_00001.flac";
    request.subfolder = "airack/00112233-4455-6677-8899-aabbccddeeff/1/"
                        "10213243-5465-7687-98a9-bacbdcedfe0f";
  }
  return request;
}
void red(const juce::File &root) {
  section = "red_binary_download";
  const std::string carrier("fLaC\0\1\0\2", 8);
  const std::string target =
      "/view?filename=seed-17_00001.flac&subfolder=airack%2F00112233-4455-6677-"
      "8899-aabbccddeeff%2F1%2F10213243-5465-7687-98a9-bacbdcedfe0f&type="
      "output";
  NativeComfyFixture fixture("GET", target, "",
                             response(carrier, "audio/flac"));
  CHECK(fixture.valid());
  auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                   fixture.port());
  detail::ComfyTestControl diagnostic;
  detail::ComfyHttpTestAccess::control(*http, &diagnostic);
  std::atomic_bool cancel{false};
  auto reply = http->perform(owned(ComfyOperation::download), cancel,
                             Clock::now() + std::chrono::seconds(3));
  if (reply.status != ComfyHttpStatus::ok)
    std::cerr << "download status=" << unsigned(reply.status)
              << " os=" << GetLastError() << " http=" << reply.httpStatus
              << " renames=" << diagnostic.renameCalls
              << " dispositions=" << diagnostic.dispositionCalls
              << " native=" << diagnostic.osError << "\n";
  CHECK(reply.status == ComfyHttpStatus::ok);
  CHECK(fixture.requestCount() == 1);
  CHECK(reply.raw != nullptr);
  if (reply.raw) {
    CHECK(reply.raw->size() == carrier.size());
    std::array<std::byte, 8> actual{};
    CHECK(reply.raw->readAt(0, actual));
    CHECK(std::memcmp(actual.data(), carrier.data(), carrier.size()) == 0);
    const auto expected =
        juce::SHA256(carrier.data(), carrier.size()).getRawData();
    CHECK(std::memcmp(reply.raw->sha256().data(), expected.getData(), 32) == 0);
    const auto path = reply.raw->file();
    reply.raw.reset();
    CHECK(!path.exists());
  }
  fixture.join();
  if (fixture.requestCount())
    CHECK(fixture.captured().matched);
  section = "red_owned_history_utf8";
  const std::string json =
      "{\"10213243-5465-7687-98a9-bacbdcedfe0f\":{\"label\":\"caf\xc3\xa9\"}}";
  NativeComfyFixture history("GET",
                             "/history/10213243-5465-7687-98a9-bacbdcedfe0f",
                             "", response(json));
  CHECK(history.valid());
  auto historyHttp = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4,
                                                          root, history.port());
  auto historyReply =
      historyHttp->perform(owned(ComfyOperation::history), cancel,
                           Clock::now() + std::chrono::seconds(3));
  CHECK(historyReply.status == ComfyHttpStatus::ok);
  CHECK(historyReply.json == json);
  CHECK(history.requestCount() == 1);
  CHECK(historyReply.raw == nullptr);
  history.join();
  if (history.requestCount())
    CHECK(history.captured().matched);
  CHECK(root.findChildFiles(juce::File::findFiles, false, "*.part").isEmpty());
}
void directoryReparseProbe(const juce::File &root) {
  const auto outside = root.getChildFile(juce::Uuid().toString() + "-outside");
  CHECK(outside.createDirectory().wasOk());
  for (bool occupied : {false, true}) {
    const auto directory =
        root.getChildFile(juce::Uuid().toString() + "-guard");
    CHECK(directory.createDirectory().wasOk());
    const auto guard = CreateFileW(
        directory.getFullPathName().toWideCharPointer(),
        FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    CHECK(guard != INVALID_HANDLE_VALUE);
    HANDLE child = INVALID_HANDLE_VALUE;
    if (occupied) {
      wchar_t name[] = L"owned.part";
      UNICODE_STRING unicode{};
      unicode.Buffer = name;
      unicode.Length = USHORT(std::wcslen(name) * 2);
      unicode.MaximumLength = unicode.Length;
      OBJECT_ATTRIBUTES attrs{};
      attrs.Length = sizeof(attrs);
      attrs.RootDirectory = guard;
      attrs.ObjectName = &unicode;
      attrs.Attributes = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE;
      IO_STATUS_BLOCK io{};
      const auto create = reinterpret_cast<decltype(&NtCreateFile)>(
          GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
      HANDLE owned = nullptr;
      const auto status =
          create
              ? create(&owned,
                       GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE,
                       &attrs, &io, nullptr, FILE_ATTRIBUTE_TEMPORARY,
                       FILE_SHARE_READ, FILE_CREATE,
                       FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                           FILE_OPEN_REPARSE_POINT,
                       nullptr, 0)
              : NTSTATUS(0xc0000001);
      child = owned ? owned : INVALID_HANDLE_VALUE;
      std::cout << "native_create_plain_probe status=" << std::hex
                << ULONG(status) << std::dec
                << " information=" << io.Information << '\n';
      CHECK(status == 0 && io.Information == FILE_CREATED &&
            child != INVALID_HANDLE_VALUE);
    }
    const auto actor = CreateFileW(
        directory.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const std::wstring target(outside.getFullPathName().toWideCharPointer());
    const auto native = L"\\??\\" + target;
    struct Junction {
      DWORD tag;
      WORD length, reserved, subOffset, subLength, printOffset, printLength;
      wchar_t names[1];
    };
    const auto bytes = (native.size() + 1 + target.size() + 1) * 2;
    std::vector<std::byte> data(offsetof(Junction, names) + bytes);
    auto *junction = reinterpret_cast<Junction *>(data.data());
    junction->tag = IO_REPARSE_TAG_MOUNT_POINT;
    junction->length = WORD(8 + bytes);
    junction->subLength = WORD(native.size() * 2);
    junction->printOffset = WORD((native.size() + 1) * 2);
    junction->printLength = WORD(target.size() * 2);
    std::memcpy(junction->names, native.c_str(), (native.size() + 1) * 2);
    std::memcpy(reinterpret_cast<std::byte *>(junction->names) +
                    junction->printOffset,
                target.c_str(), (target.size() + 1) * 2);
    DWORD returned = 0;
    SetLastError(0);
    const auto ok =
        actor != INVALID_HANDLE_VALUE &&
        DeviceIoControl(actor, FSCTL_SET_REPARSE_POINT, data.data(),
                        DWORD(data.size()), nullptr, 0, &returned, nullptr);
    const auto error = GetLastError();
    std::cout << "directory_reparse_probe occupied=" << occupied
              << " actor_open=" << (actor != INVALID_HANDLE_VALUE)
              << " set=" << ok << " error=" << error << '\n';
    if (ok) {
      const auto name = juce::Uuid().toString() + ".part";
      const std::wstring wide(name.toWideCharPointer());
      UNICODE_STRING unicode{};
      unicode.Buffer = const_cast<wchar_t *>(wide.c_str());
      unicode.Length = USHORT(wide.size() * 2);
      unicode.MaximumLength = unicode.Length;
      OBJECT_ATTRIBUTES attributes{};
      attributes.Length = sizeof(attributes);
      attributes.RootDirectory = guard;
      attributes.ObjectName = &unicode;
      attributes.Attributes = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE;
      const auto create = reinterpret_cast<decltype(&NtCreateFile)>(
          GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
      CHECK(create != nullptr);
      HANDLE created = nullptr;
      IO_STATUS_BLOCK io{};
      const auto status =
          create
              ? create(&created,
                       GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE,
                       &attributes, &io, nullptr, FILE_ATTRIBUTE_TEMPORARY,
                       FILE_SHARE_READ, FILE_CREATE,
                       FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                           FILE_OPEN_REPARSE_POINT,
                       nullptr, 0)
              : NTSTATUS(0xc0000001);
      std::cout << "native_create_reparse_probe status=" << std::hex
                << ULONG(status) << std::dec
                << " information=" << io.Information
                << " outside_exists=" << outside.getChildFile(name).exists()
                << '\n';
      CHECK(!outside.getChildFile(name).exists());
      if (created && created != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO removeFile{TRUE};
        CHECK(SetFileInformationByHandle(created, FileDispositionInfo,
                                         &removeFile, sizeof(removeFile)) != 0);
        CloseHandle(created);
      }
      struct {
        DWORD tag;
        WORD length, reserved;
      } remove{IO_REPARSE_TAG_MOUNT_POINT, 0, 0};
      CHECK(DeviceIoControl(actor, FSCTL_DELETE_REPARSE_POINT, &remove,
                            sizeof(remove), nullptr, 0, &returned,
                            nullptr) != 0);
    }
    if (actor != INVALID_HANDLE_VALUE)
      CloseHandle(actor);
    if (child != INVALID_HANDLE_VALUE) {
      FILE_DISPOSITION_INFO remove{TRUE};
      CHECK(SetFileInformationByHandle(child, FileDispositionInfo, &remove,
                                       sizeof(remove)) != 0);
      CloseHandle(child);
    }
    if (guard != INVALID_HANDLE_VALUE)
      CloseHandle(guard);
    CHECK(RemoveDirectoryW(directory.getFullPathName().toWideCharPointer()) !=
          0);
  }
  CHECK(RemoveDirectoryW(outside.getFullPathName().toWideCharPointer()) != 0);
}
void renameProbe(const juce::File &root) {
  section = "native_relative_rename_api_probe";
  std::cout << "rename_abi sizeof=" << sizeof(FILE_RENAME_INFO)
            << " root_offset=" << offsetof(FILE_RENAME_INFO, RootDirectory)
            << " length_offset=" << offsetof(FILE_RENAME_INFO, FileNameLength)
            << " name_offset=" << offsetof(FILE_RENAME_INFO, FileName)
            << " alignment=" << alignof(FILE_RENAME_INFO) << '\n';
  const auto source = root.getChildFile(juce::Uuid().toString() + ".probe");
  HANDLE file = CreateFileW(source.getFullPathName().toWideCharPointer(),
                            GENERIC_READ | GENERIC_WRITE | DELETE,
                            FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
  CHECK(file != INVALID_HANDLE_VALUE);
  if (file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  CHECK(WriteFile(file, "owned", 5, &written, nullptr) && written == 5);
  const auto directory = CreateFileW(
      root.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  CHECK(directory != INVALID_HANDLE_VALUE);
  for (int variant = 0; variant < 3; ++variant) {
    const auto name = juce::Uuid().toString() + ".probe";
    const auto path =
        variant == 1 ? root.getChildFile(name).getFullPathName() : name;
    const auto wide = path.toWideCharPointer();
    const auto n = std::wcslen(wide);
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + (n + 1) * 2);
    auto *info = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = variant == 1 ? nullptr : directory;
    info->FileNameLength = DWORD(n * 2);
    std::memcpy(info->FileName, wide, n * 2);
    if (variant < 2) {
      SetLastError(0);
      const auto ok = SetFileInformationByHandle(file, FileRenameInfo, info,
                                                 DWORD(storage.size()));
      std::cout << "rename_api_probe win32 "
                << (variant == 0 ? "relative" : "absolute") << " success=" << ok
                << " error=" << GetLastError() << '\n';
    } else {
      struct IoStatus {
        union {
          LONG status;
          void *pointer;
        };
        ULONG_PTR information;
      };
      using NtSet = LONG(NTAPI *)(HANDLE, IoStatus *, void *, ULONG, ULONG);
      const auto nt = reinterpret_cast<NtSet>(GetProcAddress(
          GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
      CHECK(nt != nullptr);
      IoStatus io{};
      const auto status = nt ? nt(file, &io, info, ULONG(storage.size()), 10)
                             : LONG(0xc0000001);
      std::cout << "rename_api_probe native_relative status=" << std::hex
                << ULONG(status) << std::dec << '\n';
    }
  }
  if (directory != INVALID_HANDLE_VALUE)
    CloseHandle(directory);
  const auto readonly = CreateFileW(
      root.getFullPathName().toWideCharPointer(),
      FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  CHECK(readonly != INVALID_HANDLE_VALUE);
  const auto name = juce::Uuid().toString() + ".probe";
  const auto wide = name.toWideCharPointer();
  const auto n = std::wcslen(wide);
  std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + (n + 1) * 2);
  auto *info = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
  info->ReplaceIfExists = FALSE;
  info->RootDirectory = readonly;
  info->FileNameLength = DWORD(n * 2);
  std::memcpy(info->FileName, wide, n * 2);
  struct NativeIo {
    union {
      LONG status;
      void *pointer;
    };
    ULONG_PTR information;
  };
  using NativeSet = LONG(NTAPI *)(HANDLE, NativeIo *, void *, ULONG, ULONG);
  const auto nt = reinterpret_cast<NativeSet>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
  NativeIo io{};
  const auto status =
      nt ? nt(file, &io, info, ULONG(storage.size()), 10) : LONG(0xc0000001);
  std::cout << "rename_api_probe native_relative_readonly_share_read status="
            << std::hex << ULONG(status) << std::dec << '\n';
  FILE_DISPOSITION_INFO disposition{TRUE};
  CHECK(SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                   sizeof(disposition)) != 0);
  CloseHandle(file);
  if (readonly != INVALID_HANDLE_VALUE)
    CloseHandle(readonly);
}
std::string chunks(const std::string &body) {
  std::string out =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: "
      "chunked\r\nConnection: close\r\n\r\n";
  for (std::size_t at = 0; at < body.size(); at += 4096) {
    char hex[32]{};
    const auto n = std::min<std::size_t>(4096, body.size() - at);
    std::snprintf(hex, sizeof(hex), "%zx", n);
    out += hex;
    out += "\r\n";
    out.append(body, at, n);
    out += "\r\n";
  }
  return out + "0\r\n\r\n";
}
void limits(const juce::File &root) {
  section = "strict_json_and_http_body_limits";
  auto run = [&](std::string wire) {
    NativeComfyFixture fixture("GET",
                               "/history/10213243-5465-7687-98a9-bacbdcedfe0f",
                               "", std::move(wire));
    CHECK(fixture.valid());
    auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                     fixture.port());
    std::atomic_bool cancel{false};
    auto reply = http->perform(owned(ComfyOperation::history), cancel,
                               Clock::now() + std::chrono::seconds(4));
    fixture.join();
    CHECK(fixture.requestCount() == 1 && fixture.captured().matched);
    return reply;
  };
  auto good = run(response("{}"));
  CHECK(good.status == ComfyHttpStatus::ok && good.json == "{}");
  for (const std::string body : std::vector<std::string>{
           "{\"x\":1,\"x\":2}", "{}x", std::string("{}\0", 3),
           std::string("\xff", 1),
           std::string(25, '[') + "0" + std::string(25, ']')}) {
    auto reply = run(response(body));
    CHECK(reply.status == ComfyHttpStatus::invalidResponse && !reply.raw &&
          reply.json.empty());
  }
  const std::string exact = "{\"x\":\"" + std::string(262136, 'a') + "\"}";
  good = run(response(exact));
  CHECK(good.status == ComfyHttpStatus::ok && good.json == exact);
  auto over = run(response(exact + " "));
  CHECK(over.status == ComfyHttpStatus::tooLarge && over.json.empty());
  good = run(chunks(exact));
  CHECK(good.status == ComfyHttpStatus::ok && good.json == exact);
  over = run(chunks(exact + " "));
  CHECK(over.status == ComfyHttpStatus::tooLarge && over.json.empty());
  for (const auto &length :
       {"-1", "18446744073709551616", "999999999999999999999999999999999999"}) {
    auto reply = run("HTTP/1.1 200 OK\r\nContent-Type: "
                     "application/json\r\nContent-Length: " +
                     std::string(length) + "\r\nConnection: close\r\n\r\n{}");
    CHECK(reply.status != ComfyHttpStatus::ok && !reply.raw &&
          reply.json.empty());
  }
  auto truncated = run(
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
      "8\r\nConnection: close\r\n\r\n{}");
  CHECK(truncated.status != ComfyHttpStatus::ok && truncated.json.empty());
  auto compressed = run(
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: "
      "gzip\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
  CHECK(compressed.status != ComfyHttpStatus::ok && compressed.json.empty());
  for (unsigned code : {400u, 404u, 405u, 409u, 500u}) {
    auto reply = run(response("{}", "application/json", code));
    CHECK(reply.status == ComfyHttpStatus::httpError &&
          reply.httpStatus == code && reply.json.empty() && !reply.raw);
  }
  section = "finite_surplus_connection_bytes_probe";
  auto surplus = run(
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
      "2\r\nConnection: close\r\n\r\n{}EXTRA");
  std::cout << "surplus_probe advertised=2 actual_on_connection=7 status="
            << unsigned(surplus.status)
            << " exposed_json_bytes=" << surplus.json.size() << '\n';
  std::cout << "original_surplus_rejection=OBSERVED_API_LIMITATION_NOT_PASS\n";
  CHECK(surplus.status == ComfyHttpStatus::ok &&
        surplus.json == "{}"); // Amended exposed HTTP-framed body contract.
  FixtureOptions isolation;
  isolation.keepOpen = true;
  isolation.replies = {
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
      "2\r\nConnection: keep-alive\r\n\r\n{}EXTRA",
      response("{\"fresh\":true}")};
  NativeComfyFixture same("GET",
                          "/history/10213243-5465-7687-98a9-bacbdcedfe0f", "",
                          response("{}"), isolation);
  CHECK(same.valid());
  auto isolated = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4,
                                                       root, same.port());
  std::atomic_bool cancel{false};
  auto first = isolated->perform(owned(ComfyOperation::history), cancel,
                                 Clock::now() + std::chrono::seconds(2));
  auto second = isolated->perform(owned(ComfyOperation::history), cancel,
                                  Clock::now() + std::chrono::seconds(2));
  same.join();
  CHECK(first.status == ComfyHttpStatus::ok && first.json == "{}");
  CHECK(second.status == ComfyHttpStatus::ok &&
        second.json == "{\"fresh\":true}");
  CHECK(same.requestCount() == 2 && same.reusedBytes() == 0);
  for (const auto &capture : same.captures())
    CHECK(capture.matched);
}

const std::string historyTarget =
    "/history/10213243-5465-7687-98a9-bacbdcedfe0f";
const std::string downloadTarget =
    "/view?filename=seed-17_00001.flac&subfolder=airack%2F00112233-4455-6677-"
    "8899-aabbccddeeff%2F1%2F10213243-5465-7687-98a9-bacbdcedfe0f&type=output";
ComfyHttpReply exchange(const juce::File &root, ComfyHttpRequest request,
                        std::string wire, FixtureOptions options = {}) {
  const bool post = request.operation == ComfyOperation::submit ||
                    request.operation == ComfyOperation::cancelOwned;
  const auto target =
      request.operation == ComfyOperation::submit ? "/prompt"
      : request.operation == ComfyOperation::cancelOwned
          ? "/api/jobs/10213243-5465-7687-98a9-bacbdcedfe0f/cancel"
      : request.operation == ComfyOperation::download ? downloadTarget
                                                      : historyTarget;
  const bool ipv6 = options.ipv6;
  NativeComfyFixture fixture(post ? "POST" : "GET", target, request.submitJson,
                             std::move(wire), std::move(options));
  CHECK(fixture.valid());
  auto http = detail::ComfyHttpTestAccess::fixture(
      ipv6 ? ComfyLoopback::ipv6 : ComfyLoopback::ipv4, root, fixture.port());
  std::atomic_bool cancel{false};
  auto reply =
      http->perform(request, cancel, Clock::now() + std::chrono::seconds(4));
  fixture.join();
  CHECK(fixture.requestCount() == 1 && fixture.captured().matched);
  const auto capture = fixture.captured();
  CHECK(capture.raw.find("Authorization:") == std::string::npos &&
        capture.raw.find("Cookie:") == std::string::npos &&
        capture.raw.find("Proxy-Authorization:") == std::string::npos);
  return reply;
}
bool networkDrained() {
  const auto end = Clock::now() + std::chrono::seconds(1);
  while (Clock::now() < end) {
    if (detail::ComfyHttpTestAccess::live() == std::array<unsigned, 2>{0, 0})
      return true;
    std::this_thread::yield();
  }
  return false;
}
void submitRejection(const juce::File &root) {
  std::cout << "rejection_stage=positive_begin" << std::endl;
  section = "real_submit400_private_rejection";
  const std::string envelope = R"({"error":{"type":"prompt_outputs_failed_validation","message":"Prompt outputs failed validation","details":"fixture validation failure","extra_info":{}},"node_errors":{"7":{"errors":[{"type":"required_input_missing","message":"Required input is missing","details":"audio","extra_info":{}}],"dependent_outputs":["7"],"class_type":"SaveAudioAdvanced"}}})";
  auto request = owned(ComfyOperation::submit);
  request.submitJson = "{}";
  auto reply = exchange(root, request, response(envelope, "application/json", 400));
  CHECK(reply.status == ComfyHttpStatus::httpError && reply.httpStatus == 400);
  CHECK(reply.submitRejected);
  CHECK(reply.requestMayHaveReachedServer && reply.json.empty() && !reply.raw);
  CHECK(networkDrained());
  for (const auto *type : {"invalid_prompt_id", "no_prompt", "missing_node_type", "prompt_no_outputs", "prompt_outputs_failed_validation"}) {
    const auto early = std::string("{\"error\":{\"type\":\"") + type + "\",\"message\":\"\",\"details\":\"\",\"extra_info\":{}},\"node_errors\":{}}";
    reply = exchange(root, request, response(early, "application/json", 400));
    CHECK(reply.status == ComfyHttpStatus::httpError && reply.httpStatus == 400 && reply.submitRejected && reply.json.empty() && !reply.raw);
  }
  section = "submit400_shape_media_and_private_negative_controls";
  std::vector<std::string> invalid{"{}", "null", "[]", envelope + "x", "{\"error\":{},\"error\":{},\"node_errors\":{}}", std::string("{\"x\":\"\0\"}", 9), "{\"x\":\"\xc3\"}", std::string(25, '[') + "0" + std::string(25, ']')};
  const auto replace = [&](std::string from, std::string to) {
    auto value = envelope; const auto p = value.find(from); CHECK(p != std::string::npos); if (p != std::string::npos) value.replace(p, from.size(), to); invalid.push_back(std::move(value));
  };
  replace("prompt_outputs_failed_validation", "unknown_rejection");
  replace("\"error\":{", "\"extra\":0,\"error\":{");
  replace("\"error\":{", "\"error\":{\"extra\":0,");
  replace("\"message\":\"Prompt outputs failed validation\"", "\"message\":null");
  replace("\"details\":\"fixture validation failure\"", "\"details\":0");
  replace("\"extra_info\":{}", "\"extra_info\":[]");
  replace("\"details\":\"audio\",\"extra_info\":{}", "\"details\":\"audio\",\"extra_info\":[]");
  invalid.push_back(R"({"error":{"type":"no_prompt","message":"","details":"","extra_info":{}},"node_errors":[]})");
  replace("\"node_errors\":{", "\"node_errors\":null,\"removed\":{");
  replace("\"7\":{", "\"\":{");
  replace("\"errors\":[{", "\"errors\":[null,{");
  replace("\"type\":\"required_input_missing\"", "\"type\":\"\"");
  replace("\"message\":\"Required input is missing\"", "\"message\":false");
  replace("\"details\":\"audio\"", "\"details\":{}");
  replace("\"dependent_outputs\":[\"7\"]", "\"dependent_outputs\":[\"\"]");
  replace("\"dependent_outputs\":[\"7\"]", "\"dependent_outputs\":[7]");
  replace("\"class_type\":\"SaveAudioAdvanced\"", "\"class_type\":\"\"");
  replace("\"class_type\":\"SaveAudioAdvanced\"", "\"class_type\":null");
  unsigned negativeOrdinal = 0;
  for (const auto &body : invalid) {
    std::cout << "rejection_negative_begin=" << negativeOrdinal << " bytes=" << body.size() << std::endl;
    reply = exchange(root, request, response(body, "application/json", 400));
    std::cout << "rejection_negative_end=" << negativeOrdinal++ << " status=" << unsigned(reply.status) << " http=" << reply.httpStatus << " signal=" << reply.submitRejected << std::endl;
    CHECK(reply.status == ComfyHttpStatus::httpError && reply.httpStatus == 400 && !reply.submitRejected && reply.json.empty() && !reply.raw);
  }
  for (const auto *media : {"text/plain", "audio/flac", "application/octet-stream", ""}) {
    reply = exchange(root, request, response(envelope, media, 400));
    CHECK(reply.status == ComfyHttpStatus::httpError && !reply.submitRejected && reply.json.empty() && !reply.raw);
  }
  for (unsigned code : {200u, 500u}) {
    reply = exchange(root, request, response(envelope, "application/json", code));
    CHECK(!reply.submitRejected && !reply.raw);
    CHECK(code == 200 ? reply.status == ComfyHttpStatus::ok && reply.json == envelope : reply.status == ComfyHttpStatus::httpError && reply.json.empty());
  }
  for (auto operation : {ComfyOperation::history, ComfyOperation::cancelOwned}) {
    reply = exchange(root, owned(operation), response(envelope, "application/json", 400));
    CHECK(reply.status == ComfyHttpStatus::httpError && !reply.submitRejected && reply.json.empty() && !reply.raw);
  }
  section = "submit400_exact_body_bounds_hostile_extra_info_and_framing";
  std::cout << "rejection_stage=exact_body_bounds_begin" << std::endl;
  auto maximum = envelope;
  const auto p = maximum.find("fixture validation failure");
  maximum.replace(p, std::strlen("fixture validation failure"), std::string(262144 - envelope.size() + std::strlen("fixture validation failure"), 'x'));
  CHECK(maximum.size() == 262144);
  reply = exchange(root, request, response(maximum, "application/json", 400));
  CHECK(reply.status == ComfyHttpStatus::httpError && reply.submitRejected && reply.json.empty());
  maximum += ' ';
  reply = exchange(root, request, response(maximum, "application/json", 400));
  CHECK(reply.status == ComfyHttpStatus::tooLarge && !reply.submitRejected && reply.json.empty());
  for (unsigned shape = 0; shape != 3; ++shape) {
    std::cout << "rejection_hostile_begin=" << shape << std::endl;
    std::string extra;
    if (shape == 0) {
      extra = "{"; for (unsigned i = 0; i != 10000; ++i) { if (i) extra += ','; extra += "\"k" + std::to_string(i) + "\":0"; } extra += '}';
    } else if (shape == 1) extra = "{\"escaped\":\"" + std::string(120000, '\\') + "\"}";
    else extra = std::string(19, '[') + "0" + std::string(19, ']');
    // extra_info remains an object, with strict bounded opaque child contents.
    if (shape == 2) extra = "{\"nested\":" + extra + "}";
    auto body = envelope; const auto at = body.find("\"extra_info\":{}"); body.replace(at, 15, "\"extra_info\":" + extra);
    CHECK(body.size() <= 262144);
    reply = exchange(root, request, response(body, "application/json", 400));
    std::cout << "rejection_hostile_end=" << shape << " status=" << unsigned(reply.status) << " signal=" << reply.submitRejected << std::endl;
    CHECK(reply.status == ComfyHttpStatus::httpError && reply.submitRejected && reply.json.empty());
  }
  for (const auto &wire : {
      "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(envelope.size() + 1) + "\r\nConnection: close\r\n\r\n" + envelope,
      "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nzz\r\n" + envelope + "\r\n0\r\n\r\n",
      "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(envelope.size()) + "\r\nContent-Encoding: gzip\r\nConnection: close\r\n\r\n" + envelope}) {
    reply = exchange(root, request, wire);
    CHECK(reply.status != ComfyHttpStatus::ok && !reply.submitRejected && reply.json.empty() && !reply.raw);
  }
  CHECK(networkDrained());
  std::cout << "rejection_stage=shapes_complete" << std::endl;
}
void submitRejectionDrain(const juce::File &root) {
  section = "submit400_actual_final_callback_eligibility";
  const std::string envelope = R"({"error":{"type":"no_prompt","message":"","details":"","extra_info":{}},"node_errors":{}})";
  for (unsigned stop = 0; stop != 6; ++stop) {
    std::cout << "rejection_drain_begin=" << stop << std::endl;
    struct Context { Barrier barrier; std::atomic_bool selected{false}, held{false}; } context;
    detail::ComfyTestControl control; control.context = &context;
    if (stop == 4) control.fault = detail::ComfyTestFault::closeReportOnce;
    if (stop == 5) control.fault = detail::ComfyTestFault::waitReportOnce;
    control.networkPoint = [](void *p, detail::ComfyNetworkTestPoint point, unsigned code) noexcept {
      auto &c = *static_cast<Context *>(p);
      if (point == detail::ComfyNetworkTestPoint::resultSelected) c.selected = true;
      if (point == detail::ComfyNetworkTestPoint::callbackLeaving && code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING && c.selected && !c.held.exchange(true)) c.barrier.reach();
    };
    auto request = owned(ComfyOperation::submit); request.submitJson = "{}";
    NativeComfyFixture fixture("POST", "/prompt", "{}", response(envelope, "application/json", 400));
    auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root, fixture.port());
    detail::ComfyHttpTestAccess::control(*http, &control);
    std::atomic_bool cancel{false};
    const auto deadline = Clock::now() + std::chrono::milliseconds(stop == 3 ? 350 : 3000);
    auto running = std::async(std::launch::async, [&] { return http->perform(request, cancel, deadline); });
    CHECK(context.barrier.waitReached());
    std::cout << "rejection_drain_held=" << stop << std::endl;
    CHECK(running.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    if (stop == 1) cancel = true;
    if (stop == 2) http->abort();
    if (stop == 3) while (Clock::now() < deadline) std::this_thread::yield();
    context.barrier.release();
    auto reply = running.get(); fixture.join();
    std::cout << "rejection_drain_end=" << stop << " status=" << unsigned(reply.status) << " signal=" << reply.submitRejected << std::endl;
    const auto expected = stop == 0 ? ComfyHttpStatus::httpError : stop <= 2 ? ComfyHttpStatus::cancelled : stop == 3 ? ComfyHttpStatus::timeout : ComfyHttpStatus::unavailable;
    CHECK(reply.status == expected && reply.httpStatus == 400 && reply.submitRejected == (stop == 0));
    CHECK(reply.requestMayHaveReachedServer && reply.json.empty() && !reply.raw);
    CHECK(fixture.captured().matched && fixture.requestCount() == 1 && networkDrained());
  }
  section = "submit400_cancel_abort_during_body";
  for (bool abort : {false, true}) {
    std::cout << "rejection_body_cancel_begin=" << abort << std::endl;
    auto request = owned(ComfyOperation::submit); request.submitJson = "{}";
    Barrier fragment; FixtureOptions options;
    const auto wire = response(envelope, "application/json", 400);
    const auto split = wire.find("\r\n\r\n") + 4 + envelope.size() / 2;
    options.segments = {wire.substr(0, split), wire.substr(split)};
    options.afterSegment = [&](unsigned i) { if (!i) fragment.reach(); };
    NativeComfyFixture fixture("POST", "/prompt", "{}", "", std::move(options));
    auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root, fixture.port());
    std::atomic_bool cancel{false};
    auto running = std::async(std::launch::async, [&] { return http->perform(request, cancel, Clock::now() + std::chrono::seconds(3)); });
    CHECK(fragment.waitReached()); if (abort) http->abort(); else cancel = true; fragment.release();
    auto reply = running.get(); fixture.join();
    std::cout << "rejection_body_cancel_end=" << abort << " status=" << unsigned(reply.status) << std::endl;
    CHECK(reply.status == ComfyHttpStatus::cancelled && !reply.submitRejected && reply.json.empty() && !reply.raw);
    CHECK(fixture.captured().matched && networkDrained());
  }
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool rejectionAllocationArmed = false;
thread_local unsigned rejectionAllocationDenials = 0;
thread_local std::size_t rejectionDeniedSize = 0;
thread_local unsigned rejectionAllocationPhase = 0;
thread_local std::array<std::array<std::size_t, 128>, 2> rejectionAllocationSizes{};
thread_local std::array<unsigned, 2> rejectionAllocationCounts{};
thread_local bool rejectionCalibrate = false;
thread_local std::size_t rejectionAllocationTarget = 0;
thread_local unsigned rejectionTargetPhase = 0;
int __cdecl rejectionAllocationHook(int kind, void *, std::size_t bytes, int use, long, const unsigned char *, int) {
  if (rejectionAllocationArmed && kind == _HOOK_ALLOC && (use & 0xffff) == _NORMAL_BLOCK && rejectionCalibrate) {
    auto &count = rejectionAllocationCounts[rejectionAllocationPhase];
    if (count < 128) rejectionAllocationSizes[rejectionAllocationPhase][count] = bytes;
    ++count; return 1;
  }
  if (rejectionAllocationArmed && kind == _HOOK_ALLOC && (use & 0xffff) == _NORMAL_BLOCK &&
      rejectionAllocationPhase == rejectionTargetPhase && bytes == rejectionAllocationTarget && !rejectionAllocationDenials) {
    ++rejectionAllocationDenials; rejectionDeniedSize = bytes; return 0;
  }
  return 1;
}
int __cdecl rejectionAllocationReport(int type, wchar_t *message, int *value) {
  if (rejectionAllocationArmed && rejectionAllocationDenials && type == _CRT_WARN && std::wstring_view(message ? message : L"") == L"Client hook allocation failure.\n") {
    if (value) *value = 0; return 1;
  }
  return 0;
}
#endif
void submitRejectionAllocationCalibration(const juce::File &root);
void submitRejectionAllocation(const juce::File &root) {
  section = "submit400_retaining_and_strict_parser_allocation_refusal";
#if defined(_MSC_VER) && defined(_DEBUG)
  submitRejectionAllocationCalibration(root);
  // The Debug MSVC image's 89-byte body requests 96 bytes of string storage.
  // Strict parsing allocates 16-byte proxies for the scanner path/numbers
  // vectors and key set before its 40-byte tree head; keep those proxies available.
  const bool calibrated = rejectionAllocationCounts[0] == 2 && rejectionAllocationSizes[0][0] == 16 &&
    rejectionAllocationSizes[0][1] == 96 && rejectionAllocationCounts[1] >= 4 &&
    rejectionAllocationSizes[1][0] == 16 && rejectionAllocationSizes[1][1] == 16 &&
    rejectionAllocationSizes[1][2] == 16 && rejectionAllocationSizes[1][3] == 40;
  CHECK(calibrated);
  if (!calibrated) return;
  section = "submit400_retaining_and_strict_parser_allocation_refusal";
  for (bool parser : {false, true}) {
    std::cout << "rejection_allocation_begin=" << parser << std::endl;
    auto request = owned(ComfyOperation::submit); request.submitJson = "{}";
    const std::string envelope = R"({"error":{"type":"no_prompt","message":"","details":"","extra_info":{}},"node_errors":{}})";
    detail::ComfyTestControl control; control.context = &parser;
    control.point = [](void *p, detail::ComfyTestPoint point, const juce::File &) noexcept {
      if (point == detail::ComfyTestPoint::headers) rejectionAllocationPhase = 0;
      if (point == detail::ComfyTestPoint::chunk) rejectionAllocationPhase = 1;
      if (point == (*static_cast<bool *>(p) ? detail::ComfyTestPoint::chunk : detail::ComfyTestPoint::headers)) rejectionAllocationArmed = true;
    };
    NativeComfyFixture fixture("POST", "/prompt", "{}", response(envelope, "application/json", 400));
    auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root, fixture.port());
    detail::ComfyHttpTestAccess::control(*http, &control);
    std::atomic_bool cancel{false}; rejectionAllocationDenials = 0; rejectionDeniedSize = 0;
    rejectionAllocationTarget = parser ? 40 : 96; rejectionTargetPhase = parser ? 1 : 0;
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, rejectionAllocationReport);
    const auto prior = _CrtSetAllocHook(rejectionAllocationHook);
    auto reply = http->perform(request, cancel, Clock::now() + std::chrono::seconds(3));
    rejectionAllocationArmed = false; _CrtSetAllocHook(prior); _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE, rejectionAllocationReport);
    fixture.join();
    std::cout << "rejection_allocation_end=" << parser << " status=" << unsigned(reply.status) << " signal=" << reply.submitRejected << std::endl;
    CHECK(rejectionAllocationDenials == 1 && rejectionDeniedSize == rejectionAllocationTarget);
    CHECK(reply.status == ComfyHttpStatus::storageFailure && !reply.submitRejected && reply.json.empty() && !reply.raw);
    CHECK(reply.requestMayHaveReachedServer && fixture.captured().matched && networkDrained());
    std::cout << "submit400_denial phase=" << (parser ? "strict_parse" : "retain_body") << " bytes=" << rejectionDeniedSize << '\n';
  }
#else
  CHECK(false && "Debug MSVC allocation refusal is required");
#endif
}
void submitRejectionAllocationCalibration(const juce::File &root) {
  section = "submit400_actual_allocation_positive_calibration";
#if defined(_MSC_VER) && defined(_DEBUG)
  auto request = owned(ComfyOperation::submit); request.submitJson = "{}";
  const std::string envelope = R"({"error":{"type":"no_prompt","message":"","details":"","extra_info":{}},"node_errors":{}})";
  detail::ComfyTestControl control;
  control.point = [](void *, detail::ComfyTestPoint point, const juce::File &) noexcept {
    if (point == detail::ComfyTestPoint::headers) { rejectionAllocationPhase = 0; rejectionAllocationArmed = true; }
    if (point == detail::ComfyTestPoint::chunk) rejectionAllocationPhase = 1;
  };
  NativeComfyFixture fixture("POST", "/prompt", "{}", response(envelope, "application/json", 400));
  auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root, fixture.port());
  detail::ComfyHttpTestAccess::control(*http, &control);
  std::atomic_bool cancel{false}; rejectionAllocationCounts = {}; rejectionAllocationSizes = {}; rejectionCalibrate = true;
  const auto prior = _CrtSetAllocHook(rejectionAllocationHook);
  auto reply = http->perform(request, cancel, Clock::now() + std::chrono::seconds(3));
  rejectionAllocationArmed = false; rejectionCalibrate = false; _CrtSetAllocHook(prior); fixture.join();
  CHECK(reply.status == ComfyHttpStatus::httpError && reply.submitRejected && reply.json.empty() && !reply.raw);
  CHECK(fixture.captured().matched && networkDrained());
  for (unsigned phase = 0; phase != 2; ++phase) {
    CHECK(rejectionAllocationCounts[phase] > 0 && rejectionAllocationCounts[phase] <= 128);
    std::cout << "rejection_actual_allocations phase=" << phase << " body_bytes=" << envelope.size() << " requests=" << rejectionAllocationCounts[phase] << " sizes=";
    for (unsigned i = 0; i != std::min(128u, rejectionAllocationCounts[phase]); ++i) std::cout << rejectionAllocationSizes[phase][i] << ',';
    std::cout << std::endl;
  }
  const auto httpSizes = rejectionAllocationSizes;
  const auto httpCounts = rejectionAllocationCounts;
  rejectionAllocationCounts = {}; rejectionAllocationSizes = {}; rejectionCalibrate = true; rejectionAllocationArmed = true;
  const auto directPrior = _CrtSetAllocHook(rejectionAllocationHook);
  rejectionAllocationPhase = 0;
  { std::string retained; retained.append(envelope); }
  rejectionAllocationPhase = 1;
  { std::set<juce::String> parserKeys; }
  rejectionAllocationArmed = false; rejectionCalibrate = false; _CrtSetAllocHook(directPrior);
  for (unsigned phase = 0; phase != 2; ++phase) {
    const auto wanted = phase ? 40u : 96u;
    unsigned hits = 0;
    for (unsigned i = 0; i != std::min(128u, rejectionAllocationCounts[phase]); ++i) hits += rejectionAllocationSizes[phase][i] == wanted;
    CHECK(hits == 1);
    std::cout << "rejection_direct_application_calibration phase=" << phase << " target=" << wanted << " exact_hits=" << hits << std::endl;
  }
  rejectionAllocationSizes = httpSizes; rejectionAllocationCounts = httpCounts;
#else
  CHECK(false && "Debug MSVC allocation calibration is required");
#endif
}
void protocols(const juce::File &root) {
  section = "routes_ipv4_ipv6_and_submit_boundary";
  for (bool ipv6 : {false, true})
    for (auto op : {ComfyOperation::submit, ComfyOperation::history,
                    ComfyOperation::cancelOwned}) {
      auto request = owned(op);
      if (op == ComfyOperation::submit)
        request.submitJson = "{\"hello\":\"caf\xc3\xa9\"}";
      FixtureOptions options;
      options.ipv6 = ipv6;
      auto reply = exchange(root, request, response("{}"), options);
      CHECK(reply.status == ComfyHttpStatus::ok && reply.json == "{}" &&
            reply.requestMayHaveReachedServer && !reply.raw);
    }
  auto submit = owned(ComfyOperation::submit);
  submit.submitJson = "{\"x\":\"" + std::string(16376, 'a') + "\"}";
  CHECK(submit.submitJson.size() == 16384);
  auto reply = exchange(root, submit, response("{}"));
  CHECK(reply.status == ComfyHttpStatus::ok);
  NativeComfyFixture unused("POST", "/prompt", "", response("{}"));
  CHECK(unused.valid());
  auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                   unused.port());
  std::atomic_bool cancel{false};
  submit.submitJson += ' ';
  reply = http->perform(submit, cancel, Clock::now() + std::chrono::seconds(1));
  unused.join();
  CHECK(reply.status == ComfyHttpStatus::tooLarge &&
        !reply.requestMayHaveReachedServer && unused.requestCount() == 0);
  section = "redirect_auth_cookie_isolation";
  for (unsigned code : {301u, 302u, 307u, 308u}) {
    NativeComfyFixture forbidden("GET", "/redirected", "", response("{}"));
    CHECK(forbidden.valid());
    const auto wire =
        "HTTP/1.1 " + std::to_string(code) +
        " Redirect\r\nLocation: http://127.0.0.1:" +
        std::to_string(forbidden.port()) +
        "/redirected\r\nContent-Type: application/json\r\nContent-Length: "
        "2\r\nConnection: close\r\n\r\n{}";
    reply = exchange(root, owned(ComfyOperation::history), wire);
    forbidden.join();
    const bool blocked = reply.status == ComfyHttpStatus::httpError &&
                         reply.httpStatus == code &&
                         forbidden.requestCount() == 0;
    CHECK(blocked);
    if (!blocked)
      return; // A redirect mutant is killed on owned loopback before foreign
              // destinations are exercised.
  }
  for (const auto &location :
       {"http://example.invalid/foreign",
        "http://user:password@127.0.0.1:8189/foreign",
        "https://127.0.0.1:8189/foreign", "//example.invalid/foreign"}) {
    reply =
        exchange(root, owned(ComfyOperation::history),
                 "HTTP/1.1 302 Found\r\nLocation: " + std::string(location) +
                     "\r\nContent-Type: application/json\r\nContent-Length: "
                     "2\r\nConnection: close\r\n\r\n{}");
    CHECK(reply.status == ComfyHttpStatus::httpError &&
          reply.httpStatus == 302 && reply.json.empty());
  }
  for (const auto &scheme : {"Basic realm=\"local\"", "Negotiate", "NTLM",
                             "Digest realm=\"local\""}) {
    reply =
        exchange(root, owned(ComfyOperation::history),
                 "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: " +
                     std::string(scheme) +
                     "\r\nContent-Type: application/json\r\nContent-Length: "
                     "2\r\nConnection: close\r\n\r\n{}");
    CHECK(reply.status == ComfyHttpStatus::httpError &&
          reply.httpStatus == 401);
  }
  FixtureOptions options;
  options.replies = {
      "HTTP/1.1 200 OK\r\nSet-Cookie: secret=do-not-replay\r\nContent-Type: "
      "application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}",
      response("{}")};
  NativeComfyFixture cookie("GET", historyTarget, "", response("{}"), options);
  CHECK(cookie.valid());
  http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                              cookie.port());
  for (unsigned n = 0; n < 2; ++n) {
    reply = http->perform(owned(ComfyOperation::history), cancel,
                          Clock::now() + std::chrono::seconds(2));
    CHECK(reply.status == ComfyHttpStatus::ok);
  }
  cookie.join();
  CHECK(cookie.requestCount() == 2);
  for (const auto &capture : cookie.captures()) {
    CHECK(capture.matched);
    CHECK(capture.raw.find("Cookie:") == std::string::npos &&
          capture.raw.find("Authorization:") == std::string::npos &&
          capture.raw.find("Proxy-Authorization:") == std::string::npos);
  }
  for (unsigned code : {408u, 500u}) {
    auto request = owned(ComfyOperation::submit);
    request.submitJson = "{}";
    auto result =
        exchange(root, request, response("{}", "application/json", code));
    CHECK(result.status == ComfyHttpStatus::httpError &&
          result.requestMayHaveReachedServer);
  }
  auto request = owned(ComfyOperation::submit);
  request.submitJson = "{}";
  auto dropped = exchange(root, request, "");
  CHECK(dropped.status != ComfyHttpStatus::ok &&
        dropped.requestMayHaveReachedServer && !dropped.raw &&
        dropped.json.empty());
  CHECK(networkDrained());
}
void headerLimits(const juce::File &root) {
  section = "header_exact_and_malformed";
  auto run = [&](const std::string &wire) {
    return exchange(root, owned(ComfyOperation::history), wire);
  };
  const std::string prefix =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
      "2\r\nConnection: close\r\nX-Pad: ";
  for (std::size_t total : {32768u, 32769u}) {
    const auto wire =
        prefix + std::string(total - prefix.size() - 4, 'a') + "\r\n\r\n{}";
    auto reply = run(wire);
    CHECK(total == 32768 ? reply.status == ComfyHttpStatus::ok
                         : reply.status != ComfyHttpStatus::ok);
  }
  for (const auto &fields :
       {"Content-Length: 2\r\nContent-Length: 2\r\n",
        "Content-Length: 2\r\nTransfer-Encoding: chunked\r\n",
        "Content-Length: +2\r\n", "Content-Length: two\r\n",
        "Transfer-Encoding: gzip\r\n",
        "Content-Length: 2\r\nContent-Type: text/plain\r\n",
        "Content-Length: 2\r\nContent-Encoding: identity\r\nContent-Encoding: "
        "identity\r\n"}) {
    auto reply = run("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" +
                     std::string(fields) + "Connection: close\r\n\r\n{}");
    CHECK(reply.status != ComfyHttpStatus::ok && reply.json.empty() &&
          !reply.raw);
  }
  for (const auto &wire : {"NOT-HTTP\r\n\r\n{}",
                           "HTTP/1.1 BAD Broken\r\nContent-Length: 2\r\n\r\n{}",
                           "HTTP/1.1 200 OK\r\nContent-Type: "
                           "application/json\r\nTransfer-Encoding: "
                           "chunked\r\n\r\n4\r\n{}\r\n0\r\n\r\n",
                           "HTTP/1.1 200 OK\r\nContent-Type: "
                           "application/json\r\nTransfer-Encoding: "
                           "chunked\r\n\r\nzz\r\n{}\r\n0\r\n\r\n"}) {
    auto reply = run(wire);
    CHECK(reply.status != ComfyHttpStatus::ok && reply.json.empty() &&
          !reply.raw);
  }
  auto disconnected = run("");
  CHECK(disconnected.status != ComfyHttpStatus::ok &&
        disconnected.requestMayHaveReachedServer);
  // This owned port is closed before the only connection attempt.
  std::uint16_t port = 0;
  {
    NativeComfyFixture fixture("GET", historyTarget, "", response("{}"));
    CHECK(fixture.valid());
    port = fixture.port();
    fixture.join();
  }
  auto http =
      detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root, port);
  std::atomic_bool cancelled{false};
  auto reply = http->perform(owned(ComfyOperation::history), cancelled,
                             Clock::now() + std::chrono::seconds(4));
  CHECK(reply.status == ComfyHttpStatus::unavailable &&
        reply.requestMayHaveReachedServer && !reply.raw && reply.json.empty());
  CHECK(networkDrained());
}
void cancellations(const juce::File &root) {
  section = "active_accept_read_cancel_abort_deadline";
  for (bool read : {false, true})
    for (unsigned action = 0; action < 3; ++action) {
      Barrier barrier;
      FixtureOptions options;
      if (read) {
        options.segments = {"HTTP/1.1 200 OK\r\nContent-Type: "
                            "application/json\r\nContent-Length: "
                            "100\r\nConnection: close\r\n\r\n{",
                            std::string(99, ' ')};
        options.afterSegment = [&](unsigned n) {
          if (n == 0)
            barrier.reach();
        };
      } else
        options.accepted = [&] { barrier.reach(); };
      NativeComfyFixture fixture("POST", "/prompt", "{}", response("{}"),
                                 options);
      CHECK(fixture.valid());
      auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4,
                                                       root, fixture.port());
      std::atomic_bool cancel{false};
      auto request = owned(ComfyOperation::submit);
      request.submitJson = "{}";
      const auto deadline =
          Clock::now() + std::chrono::milliseconds(action == 2 ? 250 : 2000);
      auto worker = std::async(std::launch::async, [&] {
        return http->perform(request, cancel, deadline);
      });
      CHECK(barrier.waitReached());
      const auto start = Clock::now();
      if (action == 0)
        cancel = true;
      else if (action == 1) {
        http->abort();
        http->abort();
      }
      CHECK(worker.wait_for(std::chrono::milliseconds(600)) ==
            std::future_status::ready);
      auto reply = worker.get();
      CHECK(reply.status == (action == 2 ? ComfyHttpStatus::timeout
                                         : ComfyHttpStatus::cancelled));
      CHECK(reply.requestMayHaveReachedServer && !reply.raw &&
            reply.json.empty());
      CHECK(Clock::now() - start < std::chrono::milliseconds(600));
      barrier.release();
      fixture.join();
      CHECK(fixture.requestCount() == 1);
      http.reset();
      CHECK(networkDrained());
    }
  section = "pre_send_cancel_event_barrier";
  NativeComfyFixture fixture("GET", historyTarget, "", response("{}"));
  CHECK(fixture.valid());
  auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                   fixture.port());
  Barrier barrier;
  detail::ComfyTestControl control;
  control.context = &barrier;
  control.point = [](void *c, detail::ComfyTestPoint p,
                     const juce::File &) noexcept {
    if (p == detail::ComfyTestPoint::beforeSend)
      static_cast<Barrier *>(c)->reach();
  };
  detail::ComfyHttpTestAccess::control(*http, &control);
  std::atomic_bool cancel{false};
  auto worker = std::async(std::launch::async, [&] {
    return http->perform(owned(ComfyOperation::history), cancel,
                         Clock::now() + std::chrono::seconds(2));
  });
  CHECK(barrier.waitReached());
  cancel = true;
  http->abort();
  barrier.release();
  CHECK(worker.wait_for(std::chrono::milliseconds(600)) ==
        std::future_status::ready);
  auto reply = worker.get();
  CHECK(reply.status == ComfyHttpStatus::cancelled &&
        !reply.requestMayHaveReachedServer);
  fixture.join();
  CHECK(fixture.requestCount() == 0);
  http.reset();
  CHECK(networkDrained());
}

void binaryLimits(const juce::File &root) {
  section = "binary_exact_chunked_unknown_and_signature";
  const std::string carrier = "fLaC" + std::string(16777212, 'x');
  for (bool chunked : {false, true})
    for (bool over : {false, true}) {
      const auto body = carrier + (over ? "x" : "");
      auto wire = chunked ? chunks(body) : response(body, "audio/flac");
      if (chunked)
        wire.replace(wire.find("application/json"), 16, "audio/flac");
      const auto started = Clock::now();
      auto reply =
          exchange(root, owned(ComfyOperation::download), std::move(wire));
      const auto elapsed = Clock::now() - started;
      std::cout << "binary_limit chunked=" << chunked << " over=" << over
                << " status=" << unsigned(reply.status)
                << " http=" << reply.httpStatus
                << " raw_present=" << bool(reply.raw)
                << " raw_bytes=" << (reply.raw ? reply.raw->size() : 0)
                << " elapsed_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                       .count()
                << std::endl;
      CHECK(over ? reply.status == ComfyHttpStatus::tooLarge
                 : reply.status == ComfyHttpStatus::ok);
      CHECK(over ? !reply.raw : reply.raw && reply.raw->size() == 16777216);
      if (reply.raw) {
        std::array<std::byte, 4> end{};
        CHECK(
            (reply.raw->readAt(reply.raw->size() - 4, end) &&
             end == std::array<std::byte, 4>{std::byte('x'), std::byte('x'),
                                             std::byte('x'), std::byte('x')}));
      }
    }
  for (const auto &body :
       std::vector<std::string>{"", "f", "fLa", "nope", "fLacXXXX"}) {
    auto reply = exchange(root, owned(ComfyOperation::download),
                          response(body, "audio/flac"));
    CHECK(reply.status == ComfyHttpStatus::invalidResponse && !reply.raw);
  }
  for (const auto &type : {"text/plain", "application/json", "audio/wav"}) {
    auto reply = exchange(root, owned(ComfyOperation::download),
                          response("fLaCxxxx", type));
    CHECK(reply.status == ComfyHttpStatus::invalidResponse && !reply.raw);
  }
  for (const auto &type :
       {"audio/flac", "audio/x-flac", "application/octet-stream"}) {
    auto reply =
        exchange(root, owned(ComfyOperation::download),
                 "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(type) +
                     "\r\nConnection: close\r\n\r\nfLaCxxxx");
    CHECK(reply.status == ComfyHttpStatus::ok && reply.raw &&
          reply.raw->size() == 8);
  }
  auto reply =
      exchange(root, owned(ComfyOperation::download),
               "HTTP/1.1 200 OK\r\nContent-Type: audio/flac\r\nContent-Length: "
               "12\r\nConnection: close\r\n\r\nfLaCxxxx");
  CHECK(reply.status != ComfyHttpStatus::ok && !reply.raw);
  FixtureOptions options;
  options.segments = {
      "HTTP/1.1 200 OK\r\nContent-Type: audio/flac\r\nContent-Length: "
      "8\r\nConnection: close\r\n\r\nf",
      "LaCxxxx"};
  Barrier fragment, firstRead;
  options.afterSegment = [&](unsigned n) {
    if (n == 0)
      fragment.reach();
  };
  detail::ComfyTestControl control;
  control.context = &firstRead;
  control.point = [](void *p, detail::ComfyTestPoint point,
                     const juce::File &) noexcept {
    if (point == detail::ComfyTestPoint::chunk)
      static_cast<Barrier *>(p)->reach();
  };
  NativeComfyFixture fixture("GET", downloadTarget, "", "", options);
  CHECK(fixture.valid());
  auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                   fixture.port());
  detail::ComfyHttpTestAccess::control(*http, &control);
  std::atomic_bool cancel{false};
  auto pending = std::async(std::launch::async, [&] {
    return http->perform(owned(ComfyOperation::download), cancel,
                         Clock::now() + std::chrono::seconds(4));
  });
  CHECK(fragment.waitReached());
  const bool delivered = firstRead.waitReached();
  fragment.release();
  firstRead.release();
  reply = pending.get();
  fixture.join();
  CHECK(delivered && control.firstBodyChunk == 1);
  CHECK(fixture.requestCount() == 1 && fixture.captured().matched);
  CHECK(reply.status == ComfyHttpStatus::ok && reply.raw);
  reply.raw.reset();
  CHECK(root.findChildFiles(juce::File::findFiles, false, "*.part").isEmpty());
  CHECK(networkDrained());
}
struct NativeSentinel {
  juce::File path;
  std::string bytes;
  juce::String hash;
  FILE_ID_INFO id{};
  NativeSentinel(const juce::File &f, std::string value)
      : path(f), bytes(std::move(value)),
        hash(juce::SHA256(bytes.data(), bytes.size()).toHexString()) {
    const auto h =
        CreateFileW(path.getFullPathName().toWideCharPointer(),
                    GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, CREATE_NEW, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE)
      return;
    DWORD wrote = 0;
    CHECK(WriteFile(h, bytes.data(), DWORD(bytes.size()), &wrote, nullptr) &&
          wrote == bytes.size());
    CHECK(GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id)));
    CloseHandle(h);
  }
  bool intact() const {
    const auto h = CreateFileW(
        path.getFullPathName().toWideCharPointer(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      return false;
    FILE_ID_INFO now{};
    std::string read(bytes.size(), '\0');
    DWORD got = 0;
    LARGE_INTEGER size{};
    const bool ok =
        GetFileInformationByHandleEx(h, FileIdInfo, &now, sizeof(now)) &&
        GetFileSizeEx(h, &size) && size.QuadPart == bytes.size() &&
        ReadFile(h, read.data(), DWORD(read.size()), &got, nullptr) &&
        got == bytes.size() && read == bytes &&
        juce::SHA256(read.data(), read.size()).toHexString() == hash &&
        std::memcmp(&id, &now, sizeof(id)) == 0;
    CloseHandle(h);
    return ok;
  }
  ~NativeSentinel() {
    CHECK(intact());
    CHECK(DeleteFileW(path.getFullPathName().toWideCharPointer()));
  }
};
HANDLE setJunction(const juce::File &directory, const juce::File &outside,
                   bool &installed) {
  const auto actor = CreateFileW(
      directory.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (actor == INVALID_HANDLE_VALUE) {
    installed = false;
    return actor;
  }
  const std::wstring target(outside.getFullPathName().toWideCharPointer()),
      native = L"\\??\\" + target;
  struct Junction {
    DWORD tag;
    WORD length, reserved, subOffset, subLength, printOffset, printLength;
    wchar_t names[1];
  };
  const auto bytes = (native.size() + 1 + target.size() + 1) * 2;
  std::vector<std::byte> data(offsetof(Junction, names) + bytes);
  auto *j = reinterpret_cast<Junction *>(data.data());
  j->tag = IO_REPARSE_TAG_MOUNT_POINT;
  j->length = WORD(8 + bytes);
  j->subLength = WORD(native.size() * 2);
  j->printOffset = WORD((native.size() + 1) * 2);
  j->printLength = WORD(target.size() * 2);
  std::memcpy(j->names, native.c_str(), (native.size() + 1) * 2);
  std::memcpy(reinterpret_cast<std::byte *>(j->names) + j->printOffset,
              target.c_str(), (target.size() + 1) * 2);
  DWORD returned = 0;
  installed =
      DeviceIoControl(actor, FSCTL_SET_REPARSE_POINT, data.data(),
                      DWORD(data.size()), nullptr, 0, &returned, nullptr) != 0;
  return actor;
}
void removeJunction(HANDLE h) {
  struct {
    DWORD tag;
    WORD length, reserved;
  } remove{IO_REPARSE_TAG_MOUNT_POINT, 0, 0};
  DWORD returned = 0;
  CHECK(DeviceIoControl(h, FSCTL_DELETE_REPARSE_POINT, &remove, sizeof(remove),
                        nullptr, 0, &returned, nullptr));
  CloseHandle(h);
}
void files(const juce::File &fixtureParent) {
  using Point = detail::ComfyTestPoint;
  using Fault = detail::ComfyTestFault;
  const auto root = fixtureParent.getChildFile("request"),
             outside = fixtureParent.getChildFile("outside");
  CHECK(!root.exists() && !outside.exists());
  CHECK(root.isAChildOf(fixtureParent) && outside.isAChildOf(fixtureParent));
  CHECK(root.createDirectory().wasOk() && outside.createDirectory().wasOk());
  struct DirectoryCleanup {
    juce::File root, outside;
    ~DirectoryCleanup() {
      CHECK(root.findChildFiles(juce::File::findFilesAndDirectories, false)
                .isEmpty());
      CHECK(outside.findChildFiles(juce::File::findFilesAndDirectories, false)
                .isEmpty());
      CHECK(RemoveDirectoryW(root.getFullPathName().toWideCharPointer()));
      CHECK(RemoveDirectoryW(outside.getFullPathName().toWideCharPointer()));
    }
  } cleanup{root, outside};
  section = "held_readback_bounds_and_failure_unchanged";
  const std::string body = "fLaC" + std::string(131072, 'q');
  auto download = [&](detail::ComfyTestControl &control) {
    NativeComfyFixture fixture("GET", downloadTarget, "",
                               response(body, "audio/flac"));
    CHECK(fixture.valid());
    auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                     fixture.port());
    detail::ComfyHttpTestAccess::control(*http, &control);
    std::atomic_bool cancel{false};
    auto reply = http->perform(owned(ComfyOperation::download), cancel,
                               Clock::now() + std::chrono::seconds(3));
    fixture.join();
    CHECK(control.fault == Fault::createFile
              ? fixture.requestCount() == 0
              : fixture.requestCount() == 1 && fixture.captured().matched);
    return reply;
  };
  detail::ComfyTestControl control;
  auto reply = download(control);
  CHECK(reply.status == ComfyHttpStatus::ok && reply.raw);
  if (reply.raw) {
    for (const auto range : std::vector<std::pair<std::uint64_t, std::size_t>>{
             {0, 4}, {reply.raw->size() - 1, 1}, {3, 65536}}) {
      std::vector<std::byte> out(range.second);
      CHECK(reply.raw->readAt(range.first, out));
      CHECK(std::memcmp(out.data(), body.data() + range.first, out.size()) ==
            0);
    }
    for (const auto range : std::vector<std::pair<std::uint64_t, std::size_t>>{
             {0, 0},
             {0, 65537},
             {reply.raw->size(), 1},
             {std::numeric_limits<std::uint64_t>::max(), 2},
             {reply.raw->size() - 1, 2}}) {
      std::vector<std::byte> out(range.second, std::byte(0x5a)), before = out;
      CHECK(!reply.raw->readAt(range.first, out) && out == before);
    }
    for (const auto fault : {Fault::read, Fault::shortRead, Fault::identity}) {
      control.fault = fault;
      std::array<std::byte, 4> out{};
      out.fill(std::byte(0x5a));
      const auto before = out;
      CHECK(!reply.raw->readAt(0, out) && out == before);
    }
    control.fault = Fault::none;
    const auto file = reply.raw->file();
    reply.raw.reset();
    CHECK(!file.exists());
  }
  section = "created_file_failure_transactionality";
  NativeSentinel sentinel(root.getChildFile("foreign-sentinel.bin"),
                          "foreign-fixed-bytes");
  for (const auto fault : {Fault::createFile, Fault::write, Fault::flush,
                           Fault::rename, Fault::read, Fault::shortRead}) {
    detail::ComfyTestControl injected;
    injected.fault = fault;
    reply = download(injected);
    CHECK(reply.status == ComfyHttpStatus::storageFailure && !reply.raw);
    CHECK(
        root.findChildFiles(juce::File::findFiles, false, "*.part").isEmpty());
    CHECK(sentinel.intact());
  }
  NativeSentinel target(root.getChildFile("occupied.flac"),
                        "never-replace-target");
  detail::ComfyTestControl collision;
  collision.finalBasename = L"occupied.flac";
  reply = download(collision);
  CHECK(reply.status == ComfyHttpStatus::storageFailure && !reply.raw &&
        target.intact() && sentinel.intact());
  section = "disposition_and_identity_safe_orphan";
  for (const auto fault : {Fault::disposition, Fault::identity}) {
    detail::ComfyTestControl injected;
    reply = download(injected);
    CHECK(reply.raw);
    if (reply.raw) {
      const auto path = reply.raw->file();
      injected.fault = fault;
      reply.raw.reset();
      CHECK(path.exists());
      CHECK(DeleteFileW(path.getFullPathName().toWideCharPointer()));
    }
    CHECK(sentinel.intact());
  }
  section = "actual_foreign_actor_final_boundaries";
  NativeSentinel outsideSentinel(outside.getChildFile("outside-sentinel.bin"),
                                 "outside-identity-fixed-bytes");
  for (const auto boundary : {Point::beforeRename, Point::afterRename,
                              Point::beforeDisposition, Point::afterClose}) {
    struct Actor {
      Point boundary;
      juce::File root, outside;
      NativeSentinel *sentinel;
      unsigned attempts = 0, directoryAttempts = 0, reparseAttempts = 0;
      bool blocked = false, dirBlocked = false, reparseBlocked = false,
           installed = false;
      juce::File installedPath;
    };
    Actor actor{boundary, root, outside, &sentinel};
    CHECK(!actor.outside.isAChildOf(actor.root) &&
          actor.outside.getParentDirectory() == actor.root.getParentDirectory());
    detail::ComfyTestControl guarded;
    guarded.context = &actor;
    guarded.point = [](void *p, Point point, const juce::File &file) noexcept {
      auto &a = *static_cast<Actor *>(p);
      if (point != a.boundary)
        return;
      std::thread foreign([&] {
        ++a.attempts;
        if (point == Point::afterClose) {
          a.installed =
              CopyFileW(a.sentinel->path.getFullPathName().toWideCharPointer(),
                        file.getFullPathName().toWideCharPointer(), TRUE) != 0;
          a.installedPath = file;
        } else {
          const auto moved = file.getSiblingFile(file.getFileName() + "-moved");
          const auto rename =
              MoveFileExW(file.getFullPathName().toWideCharPointer(),
                          moved.getFullPathName().toWideCharPointer(), 0);
          const auto write = CreateFileW(
              file.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
              OPEN_EXISTING, 0, nullptr);
          const auto replacement =
              file.getSiblingFile("actor-replacement-" + juce::Uuid().toString());
          const bool copied =
              CopyFileW(a.sentinel->path.getFullPathName().toWideCharPointer(),
                        replacement.getFullPathName().toWideCharPointer(),
                        TRUE) != 0;
          const bool replaced =
              copied &&
              MoveFileExW(replacement.getFullPathName().toWideCharPointer(),
                          file.getFullPathName().toWideCharPointer(),
                          MOVEFILE_REPLACE_EXISTING) != 0;
          if (copied && !replaced)
            DeleteFileW(replacement.getFullPathName().toWideCharPointer());
          a.blocked =
              !rename && write == INVALID_HANDLE_VALUE && copied && !replaced;
          if (write != INVALID_HANDLE_VALUE)
            CloseHandle(write);
        }
        ++a.directoryAttempts;
        const auto rootMove =
            a.root.getSiblingFile(a.root.getFileName() + "-moved");
        a.dirBlocked =
            MoveFileExW(a.root.getFullPathName().toWideCharPointer(),
                        rootMove.getFullPathName().toWideCharPointer(), 0) == 0;
        const auto parentWrite = CreateFileW(
            a.root.getParentDirectory().getFullPathName().toWideCharPointer(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        a.dirBlocked = a.dirBlocked && parentWrite == INVALID_HANDLE_VALUE;
        if (parentWrite != INVALID_HANDLE_VALUE)
          CloseHandle(parentWrite);
        bool installed = false;
        ++a.reparseAttempts;
        const auto junction = setJunction(a.root, a.outside, installed);
        a.reparseBlocked = !installed;
        if (installed)
          removeJunction(junction);
        else if (junction != INVALID_HANDLE_VALUE)
          CloseHandle(junction);
      });
      foreign.join();
    };
    reply = download(guarded);
    CHECK(reply.status == ComfyHttpStatus::ok && reply.raw);
    if (reply.raw) {
      std::array<std::byte, 4> data{};
      CHECK(reply.raw->readAt(0, data) &&
            std::memcmp(data.data(), "fLaC", 4) == 0);
      const auto hash = juce::SHA256(body.data(), body.size()).getRawData();
      CHECK(std::memcmp(reply.raw->sha256().data(), hash.getData(), 32) == 0);
      reply.raw.reset();
    }
    CHECK(actor.attempts == 1 && actor.directoryAttempts == 1 &&
          actor.reparseAttempts == 1);
    CHECK(actor.dirBlocked && actor.reparseBlocked);
    bool replacementSurvived = false;
    if (boundary == Point::afterClose) {
      replacementSurvived = actor.installed &&
                            actor.installedPath.loadFileAsString() ==
                                "foreign-fixed-bytes";
      CHECK(replacementSurvived);
      CHECK(DeleteFileW(
          actor.installedPath.getFullPathName().toWideCharPointer()));
    } else
      CHECK(actor.blocked);
    const bool insideIntact = sentinel.intact(), targetIntact = target.intact(),
               outsideIntact = outsideSentinel.intact();
    CHECK(insideIntact && targetIntact && outsideIntact);
    const auto boundaryName = boundary == Point::beforeRename ? "beforeRename"
                              : boundary == Point::afterRename ? "afterRename"
                              : boundary == Point::beforeDisposition
                                  ? "beforeDisposition"
                                  : "afterClose";
    std::cout << "final_boundary=" << boundaryName
              << " actor_attempts=" << actor.attempts
              << " directory_attempts=" << actor.directoryAttempts
              << " reparse_attempts=" << actor.reparseAttempts
              << " source_blocked=" << actor.blocked
              << " directory_blocked=" << actor.dirBlocked
              << " reparse_blocked=" << actor.reparseBlocked
              << " inside_bytes_hash_identity_intact=" << insideIntact
              << " target_bytes_hash_identity_intact=" << targetIntact
              << " outside_bytes_hash_identity_intact=" << outsideIntact
              << " outside_sha256=" << outsideSentinel.hash
              << " post_close_replacement_survived=" << replacementSurvived
              << '\n';
  }
  CHECK(networkDrained());
}

void directoryBoundaries(const juce::File &root) {
  section = "ordinary_root_sharing_transition_reparse_actor";
  const auto directory = root.getChildFile("transition-root"),
             outside = root.getChildFile("transition-outside");
  CHECK(directory.createDirectory().wasOk() &&
        outside.createDirectory().wasOk());
  struct OutsideCleanup {
    juce::File path;
    ~OutsideCleanup() {
      CHECK(RemoveDirectoryW(path.getFullPathName().toWideCharPointer()));
    }
  } cleanup{outside};
  NativeSentinel sentinel(outside.getChildFile("outside-sentinel.bin"),
                          "outside-never-change");
  struct Context {
    juce::File root, outside;
    HANDLE actor = INVALID_HANDLE_VALUE;
    unsigned attempts = 0;
    bool installed = false, renameBlocked = false;
  };
  Context context{directory, outside};
  detail::ComfyTestControl control;
  control.context = &context;
  control.point = [](void *p, detail::ComfyTestPoint point,
                     const juce::File &) noexcept {
    if (point != detail::ComfyTestPoint::rootTransition)
      return;
    auto &c = *static_cast<Context *>(p);
    std::thread actor([&] {
      ++c.attempts;
      c.renameBlocked =
          MoveFileExW(c.root.getFullPathName().toWideCharPointer(),
                      c.root.getSiblingFile("transition-stolen")
                          .getFullPathName()
                          .toWideCharPointer(),
                      0) == 0;
      c.actor = setJunction(c.root, c.outside, c.installed);
    });
    actor.join();
  };
  auto attempt = [&](const juce::File &where,
                     detail::ComfyTestControl *access) {
    NativeComfyFixture fixture("GET", downloadTarget, "",
                               response("fLaCxxxx", "audio/flac"));
    CHECK(fixture.valid());
    auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, where,
                                                     fixture.port());
    detail::ComfyHttpTestAccess::control(*http, access);
    std::atomic_bool cancel{false};
    auto reply = http->perform(owned(ComfyOperation::download), cancel,
                               Clock::now() + std::chrono::seconds(2));
    fixture.join();
    CHECK(reply.status == ComfyHttpStatus::storageFailure && !reply.raw &&
          !reply.requestMayHaveReachedServer && fixture.requestCount() == 0);
  };
  attempt(directory, &control);
  CHECK(context.attempts == 1 && context.installed && context.renameBlocked);
  CHECK(
      sentinel.intact() &&
      outside.findChildFiles(juce::File::findFiles, false, "*.part").isEmpty());
  if (context.installed)
    removeJunction(context.actor);
  else if (context.actor != INVALID_HANDLE_VALUE)
    CloseHandle(context.actor);
  for (bool ancestor : {false, true}) {
    bool installed = false;
    const auto actor = setJunction(directory, outside, installed);
    CHECK(installed);
    if (installed) {
      attempt(ancestor ? directory.getChildFile("leaf") : directory, nullptr);
      CHECK(sentinel.intact());
      removeJunction(actor);
    } else if (actor != INVALID_HANDLE_VALUE)
      CloseHandle(actor);
  }
  CHECK(RemoveDirectoryW(
      directory.getFullPathName()
          .toWideCharPointer())); // sentinel owns cleanup; outside stays until
                                  // its destructor.
}
void fileCancellation(const juce::File &root) {
  using Point = detail::ComfyTestPoint;
  section = "binary_boundary_cancellation_no_late_rename";
  for (const auto boundary : {Point::sendPending, Point::chunk,
                              Point::beforeRename, Point::afterRename}) {
    NativeComfyFixture fixture(
        "GET", downloadTarget, "",
        response("fLaC" + std::string(131072, 'k'), "audio/flac"));
    CHECK(fixture.valid());
    auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                     fixture.port());
    std::atomic_bool cancel{false};
    struct Context {
      Point boundary;
      std::atomic_bool *cancel;
      unsigned hits = 0;
    };
    Context context{boundary, &cancel};
    detail::ComfyTestControl control;
    control.context = &context;
    control.point = [](void *p, Point point, const juce::File &) noexcept {
      auto &c = *static_cast<Context *>(p);
      if (point == c.boundary) {
        ++c.hits;
        c.cancel->store(true);
      }
    };
    detail::ComfyHttpTestAccess::control(*http, &control);
    auto reply = http->perform(owned(ComfyOperation::download), cancel,
                               Clock::now() + std::chrono::seconds(2));
    fixture.join();
    CHECK(reply.status == ComfyHttpStatus::cancelled && !reply.raw &&
          reply.json.empty() && reply.requestMayHaveReachedServer);
    CHECK(context.hits == 1);
    CHECK(control.renameCalls == (boundary == Point::afterRename ? 1u : 0u));
    CHECK(fixture.requestCount() <= 1);
    CHECK(
        root.findChildFiles(juce::File::findFiles, false, "*.part").isEmpty() &&
        root.findChildFiles(juce::File::findFiles, false, "*.flac").isEmpty());
    http.reset();
    CHECK(networkDrained());
  }
}
void security(const juce::File &root) {
  section = "security_invalid_request_before_connection";
  auto rejected = [&](ComfyHttpRequest request,
                      ComfyLoopback loopback = ComfyLoopback::ipv4) {
    NativeComfyFixture fixture("GET", "/must-not-connect", "", response("{}"));
    CHECK(fixture.valid());
    auto http =
        detail::ComfyHttpTestAccess::fixture(loopback, root, fixture.port());
    std::atomic_bool cancel{false};
    auto reply =
        http->perform(request, cancel, Clock::now() + std::chrono::seconds(2));
    CHECK(reply.status == ComfyHttpStatus::invalidRequest);
    CHECK(!reply.requestMayHaveReachedServer && !reply.raw &&
          reply.json.empty());
    fixture.join();
    CHECK(fixture.requestCount() == 0 && fixture.exited());
  };
  auto request = owned(ComfyOperation::history);
  rejected(request, static_cast<ComfyLoopback>(255));
  request.operation = static_cast<ComfyOperation>(255);
  rejected(request);
  request = owned(ComfyOperation::history);
  request.owned.runtime = juce::Uuid::null();
  rejected(request);
  request = owned(ComfyOperation::history);
  request.owned.prompt = juce::Uuid::null();
  rejected(request);
  request = owned(ComfyOperation::history);
  request.owned.request = 0;
  rejected(request);
  for (auto operation :
       {ComfyOperation::history, ComfyOperation::cancelOwned}) {
    request = owned(operation);
    request.submitJson = "{}";
    rejected(request);
    request = owned(operation);
    request.filename = "seed-17_00001.flac";
    rejected(request);
    request = owned(operation);
    request.subfolder = "airack";
    rejected(request);
  }
  for (const std::string name :
       {"", "../seed-17_00001.flac", "sub/seed-17_00001.flac",
        "sub\\seed-17_00001.flac", "C:seed-17_00001.flac", "seed-17_%30.flac",
        "seed-18_00001.flac", "seed-17_.flac", "seed-17_1.wav",
        "seed-17_1.flac.exe", "seed-17_1.flac?x=1", "seed-17_1.flac#x",
        "seed-17_1.flac\r\nAuthorization: x"}) {
    request = owned(ComfyOperation::download);
    request.filename = name;
    rejected(request);
  }
  request = owned(ComfyOperation::download);
  request.filename = std::string("seed-17_1\0.flac", 15);
  rejected(request);
  const auto proper = owned(ComfyOperation::download).subfolder;
  for (const auto &folder : std::vector<std::string>{
           "", "/" + proper, "../" + proper, proper + "/..", proper + "/",
           proper + "%2f..", proper + ":stream", "airack//" + proper.substr(7),
           "airack/00112233-4455-6677-8899-aabbccddeeff/2/"
           "10213243-5465-7687-98a9-bacbdcedfe0f",
           "airack/ffffffff-ffff-ffff-ffff-ffffffffffff/1/"
           "10213243-5465-7687-98a9-bacbdcedfe0f"}) {
    request = owned(ComfyOperation::download);
    request.subfolder = folder;
    rejected(request);
  }
  request = owned(ComfyOperation::download);
  request.submitJson = "{}";
  rejected(request);
  request = owned(ComfyOperation::submit);
  request.submitJson = "{}";
  request.filename = "foreign.flac";
  rejected(request);
  request = owned(ComfyOperation::submit);
  request.submitJson = "{}";
  request.subfolder = proper;
  rejected(request);
  section = "security_sticky_local_cancel_and_expired_deadline";
  NativeComfyFixture fixture("GET", "/must-not-connect", "", response("{}"));
  CHECK(fixture.valid());
  auto http = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                   fixture.port());
  std::atomic_bool cancel{true};
  auto reply = http->perform(owned(ComfyOperation::history), cancel,
                             Clock::now() + std::chrono::seconds(2));
  CHECK(reply.status == ComfyHttpStatus::cancelled &&
        !reply.requestMayHaveReachedServer && !reply.raw);
  cancel = false;
  auto expired = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                      fixture.port());
  reply = expired->perform(owned(ComfyOperation::history), cancel,
                           Clock::now() - std::chrono::milliseconds(1));
  CHECK(reply.status == ComfyHttpStatus::timeout &&
        !reply.requestMayHaveReachedServer && !reply.raw);
  auto aborted = detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4, root,
                                                      fixture.port());
  aborted->abort();
  aborted->abort();
  reply = aborted->perform(owned(ComfyOperation::history), cancel,
                           Clock::now() + std::chrono::seconds(2));
  CHECK(reply.status == ComfyHttpStatus::cancelled &&
        !reply.requestMayHaveReachedServer && !reply.raw);
  fixture.join();
  CHECK(fixture.requestCount() == 0 && fixture.exited());
}


bool drainSetupFailed = false;
void callbackDrain(const juce::File &root) {
  using Case = ComfyUnloadCase;
  for (unsigned value = 0; value < 19; ++value) {
    const auto scenario = Case(value);
    section = "actual_owned_DLL_session_callback_drain";
    const bool binary = scenario == Case::downloadSuccess ||
        scenario == Case::downloadCancelDuringDrain ||
        scenario == Case::downloadAbortDuringDrain ||
        scenario == Case::downloadDeadlineDuringDrain;
    const bool completedStop = scenario >= Case::historyCancelDuringDrain;
    const bool pending = scenario == Case::cancelPending ||
        scenario == Case::timeoutPending || scenario == Case::abortRace;
    const bool preflight = scenario == Case::invalidRequest ||
        scenario == Case::preCancelled || scenario == Case::expired ||
        scenario == Case::createEventFailure || scenario == Case::armEventFailure;
    const bool noSend = preflight || scenario == Case::setupAfterArmFailure;
    const bool gated = !noSend && (!pending || scenario == Case::abortRace);
    const auto baseline = failures.load();
    const auto module = LoadLibraryW(RACKSYNTH_COMFY_HTTP_UNLOAD_TEST_DLL);
    const auto run = module ? reinterpret_cast<ComfyUnloadRun>(
        GetProcAddress(module, "ComfyHttpUnloadOwnedRun")) : nullptr;
    if (!module || !run) { drainSetupFailed = true; CHECK(false); return; }
    std::array<HANDLE, 10> events{};
    bool valid = true;
    for (auto &event : events) {
      event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      valid = valid && event != nullptr;
    }
    if (!valid) { drainSetupFailed = true; CHECK(false); return; }
    FixtureOptions options;
    if (pending) {
      options.keepOpen = true;
      options.beforeReply = [&] { SetEvent(events[9]); };
    }
    NativeComfyFixture fixture("GET", binary ? downloadTarget : historyTarget,
        "", pending ? "" : response(binary ? "fLaCxxxx" : "{\"ok\":17}",
                                     binary ? "audio/flac" : "application/json"), options);
    if (!fixture.valid()) { drainSetupFailed = true; CHECK(false); return; }
    NativeSentinel sentinel(root.getChildFile("drain-sentinel.bin"), "foreign-drain-fixed-bytes");
    const std::wstring directory(root.getFullPathName().toWideCharPointer());
    ComfyUnloadInput input{sizeof(ComfyUnloadInput), scenario, fixture.port(), directory.c_str(),
        events[0], events[1], events[2], events[3], events[4], events[5], events[6], events[7], events[8], TRUE};
    if (!gated) SetEvent(input.callbackRelease);
    ComfyUnloadWitness witness{sizeof(ComfyUnloadWitness)};
    const auto started = Clock::now();
    auto worker = std::async(std::launch::async, [&] { return run(&input, &witness); });
    if (!preflight) {
      const bool ready = WaitForSingleObject(input.sessionObserved, 3000) == WAIT_OBJECT_0;
      CHECK(ready);
      if (!ready) drainSetupFailed = true;
      HMODULE callbackModule = nullptr;
      CHECK(witness.sessionEvent && GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(witness.callbackAddress), &callbackModule) && callbackModule == module);
    }
    if (pending) {
      CHECK(WaitForSingleObject(events[9], 2000) == WAIT_OBJECT_0);
      if (scenario != Case::timeoutPending) SetEvent(input.abortStart);
    }
    if (gated) {
      const bool entered = WaitForSingleObject(input.callbackEntered, 3000) == WAIT_OBJECT_0;
      CHECK(entered);
      if (!entered) drainSetupFailed = true;
      if (completedStop) {
        const bool complete = WaitForSingleObject(input.completedBody, 1000) == WAIT_OBJECT_0;
        CHECK(complete);
        if (!complete || Clock::now() - started >= std::chrono::seconds(2)) drainSetupFailed = true;
        if (binary) {
          const auto raw = root.findChildFiles(juce::File::findFiles, false, "*.flac");
          CHECK(raw.size() == 1 && raw[0].getSize() == 8);
        }
        SetEvent(input.abortStart);
        CHECK(WaitForSingleObject(input.stopApplied, 3000) == WAIT_OBJECT_0);
      }
      CHECK(WaitForSingleObject(witness.sessionEvent, 0) == WAIT_TIMEOUT);
      CHECK(WaitForSingleObject(input.performReturned, 50) == WAIT_TIMEOUT);
      if (WaitForSingleObject(input.ownerRetireAttempted, 0) == WAIT_OBJECT_0) {
        section = "owner_retired_before_session_drain";
        CHECK(false);
      }
    }
    SetEvent(input.callbackRelease);
    SetEvent(input.abortStart);
    const auto result = worker.get();
    const bool safeToUnload = !witness.sessionEvent ||
        WaitForSingleObject(witness.sessionEvent, 0) == WAIT_OBJECT_0;
    if (!safeToUnload) WaitForSingleObject(witness.sessionEvent, INFINITE);
    const bool unloaded = FreeLibrary(module) != FALSE;
    const bool moduleAbsent = GetModuleHandleW(RACKSYNTH_COMFY_HTTP_UNLOAD_TEST_DLL) == nullptr;
    const auto elapsed = Clock::now() - started;
    fixture.join();
    if (result == 2) drainSetupFailed = true;
    CHECK(result == 0);
    CHECK(witness.proofAtPerformReturn == 1);
    if (!noSend) CHECK(witness.allHandlesClosed == 3);
    auto expected = ComfyHttpStatus::ok;
    if (scenario == Case::invalidRequest) expected = ComfyHttpStatus::invalidRequest;
    else if (scenario == Case::preCancelled || scenario == Case::cancelPending || scenario == Case::abortRace ||
             (completedStop && scenario < Case::historyDeadlineDuringDrain)) expected = ComfyHttpStatus::cancelled;
    else if (scenario == Case::expired || scenario == Case::timeoutPending || scenario == Case::historyDeadlineDuringDrain ||
             scenario == Case::downloadDeadlineDuringDrain) expected = ComfyHttpStatus::timeout;
    else if (scenario == Case::createEventFailure || scenario == Case::armEventFailure ||
             scenario == Case::setupAfterArmFailure || scenario == Case::closeReportFailure ||
             scenario == Case::waitReportFailure) expected = ComfyHttpStatus::unavailable;
    CHECK(witness.status == unsigned(expected));
    CHECK(witness.mayHaveReachedServer == (noSend ? 0u : 1u));
    CHECK(witness.rawSize == (scenario == Case::downloadSuccess ? 8u : 0u));
    CHECK(fixture.requestCount() == (noSend ? 0u : 1u));
    if (!noSend) CHECK(fixture.captured().matched);
    if (witness.sessionEvent) CHECK(WaitForSingleObject(witness.sessionEvent, 0) == WAIT_OBJECT_0);
    else CHECK(preflight && witness.callbackAddress == 0);
    if (scenario == Case::waitReportFailure) CHECK(witness.waitErrors == 1);
    if (scenario == Case::cancelPending) CHECK(elapsed < std::chrono::milliseconds(600));
    if (scenario == Case::timeoutPending) CHECK(elapsed >= std::chrono::milliseconds(250) && elapsed < std::chrono::milliseconds(600));
    CHECK(sentinel.intact());
    CHECK(root.findChildFiles(juce::File::findFiles, false, "*.part").isEmpty());
    CHECK(root.findChildFiles(juce::File::findFiles, false, "*.flac").isEmpty());
    CHECK(safeToUnload && unloaded && moduleAbsent);
    if (witness.sessionEvent && witness.sessionEvent != input.baselineSessionEvent) CloseHandle(witness.sessionEvent);
    for (auto event : events) CloseHandle(event);
    std::cout << "owned_dll_case=" << unsigned(scenario) << " result=" << result
              << " proof=" << witness.proofAtPerformReturn << " close_count=" << witness.allHandlesClosed
              << " failures=" << failures - baseline << '\n';
  }
}
void ownedFixturePhases(const juce::File &) {
  section = "owned_fixture_phases";
  std::atomic_uint expected{0};
  FixtureOptions options;
  options.fixedProtocol8189 = true;
  options.ownedHostReply = [&](const Capture &request) {
    const auto body = std::to_string(expected.load());
    const bool matched = request.complete && request.method == "POST" &&
                         request.target == "/owned-phase" && request.body == body;
    if (matched) ++expected;
    return OwnedHostReply{matched, response("{}")};
  };
  {
    NativeComfyFixture fixture("", "", "", "", std::move(options));
    if (!CHECK(fixture.valid() && fixture.port() == 8189)) {
      drainSetupFailed = true; // Occupied port is setup, never behavior evidence.
      return;
    }
    const auto exchange = [&] {
      const auto body = std::to_string(expected.load());
      const auto request = "POST /owned-phase HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      std::string actual;
      CHECK(independentExchange(fixture.port(), request, actual));
      CHECK(actual == response("{}"));
    };
    exchange();
    CHECK(!fixture.exited() && fixture.requestCount() == 1);
    for (unsigned phase = 2; phase <= 4; ++phase) {
      if (!CHECK(fixture.rearmOwned())) return;
      CHECK(!fixture.exited());
      exchange();
    }
    // The fifth phase is refused before the final valid phase has ended.
    CHECK(fixture.requestCount() == 4);
    CHECK(!fixture.rearmOwned());
    // The aggregate request cap is independent of the phase cap.
    for (unsigned operation = 4; operation < 64; ++operation) exchange();
    fixture.join();
    CHECK(fixture.exited() && fixture.requestCount() == 64 && expected == 64);
    CHECK(!fixture.rearmOwned());
    const auto records = fixture.captures();
    CHECK(records.size() == 64);
    for (unsigned index = 0; index < records.size(); ++index)
      CHECK(records[index].matched && records[index].body == std::to_string(index));
  }
  FixtureOptions ephemeral;
  ephemeral.ownedHostReply = [](const Capture &request) {
    return OwnedHostReply{request.complete && request.method == "GET" && request.target == "/control",
                          response("control")};
  };
  NativeComfyFixture control("", "", "", "", std::move(ephemeral));
  CHECK(control.valid() && control.port() != 8189);
  CHECK(!control.rearmOwned());
  std::string actual;
  CHECK(independentExchange(control.port(), "GET /control HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", actual));
  CHECK(actual == response("control"));
  control.join();
  CHECK(control.requestCount() == 1 && control.captured().matched);
}
} // namespace
int main(int argc, char **argv) {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return 2;
  struct Cleanup {
    ~Cleanup() { WSACleanup(); }
  } cleanup;
  std::string selected = "all";
  if (argc == 3 && std::string(argv[1]) == "--section")
    selected = argv[2];
  else if (argc != 1)
    return 2;
  if (!setupControls()) {
    std::cerr << "SETUP_FAILURE: not behavioral RED\n";
    return 2;
  }
  RequestRoot root;
  if (!root.valid) {
    std::cerr << "SETUP_FAILURE: diagnostic request root\n";
    return 2;
  }
  if (selected != "red" && selected != "security" && selected != "files" &&
      selected != "limits" && selected != "cancel" && selected != "protocol" &&
      selected != "headers" && selected != "drain" && selected != "owned-fixture" && selected != "rejection" && selected != "rejection-shapes" && selected != "rejection-drain" && selected != "rejection-allocation" && selected != "rejection-allocation-calibration" && selected != "all") {
    std::cerr << "SETUP_FAILURE: later sections not implemented in initial RED "
                 "fixture\n";
    return 2;
  }
  const auto run = [&](const char *name, auto test) {
    if (selected == name || selected == "all") {
      if (!setupControls())
        return false;
      test(root.directory);
    }
    return true;
  };
  if ((selected == "owned-fixture" && !run("owned-fixture", ownedFixturePhases)) || (selected == "rejection-allocation-calibration" && !run("rejection-allocation-calibration", submitRejectionAllocationCalibration)) || !run("rejection-shapes", submitRejection) || !run("rejection-drain", submitRejectionDrain) || !run("rejection-allocation", submitRejectionAllocation) ||
      (selected == "rejection" && !run("rejection", [](const juce::File &r) { submitRejection(r); submitRejectionDrain(r); submitRejectionAllocation(r); })) || !run("drain", callbackDrain) || !run("red", red) || !run("security", security) ||
      !run("protocol", protocols) || !run("headers", headerLimits) ||
      !run("limits",
           [](const juce::File &r) {
             limits(r);
             binaryLimits(r);
           }) ||
      !run("cancel",
           [](const juce::File &r) {
             cancellations(r);
             fileCancellation(r);
           }) ||
      !run("files", [](const juce::File &r) {
        renameProbe(r);
        directoryReparseProbe(r);
        files(r);
        directoryBoundaries(r);
      }))
    return 2;
  CHECK(networkDrained());
  CHECK(
      root.directory.findChildFiles(juce::File::findFilesAndDirectories, false)
          .isEmpty());
  std::cout << "Comfy HTTP native: " << checks << " checks, " << failures
            << " failures\n";
  return drainSetupFailed ? 2 : failures ? 1 : 0;
}
