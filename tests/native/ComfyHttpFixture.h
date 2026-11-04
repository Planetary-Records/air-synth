#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace comfy_fixture {
using Clock = std::chrono::steady_clock;
struct Socket {
  SOCKET value = INVALID_SOCKET;
  explicit Socket(SOCKET s = INVALID_SOCKET) noexcept : value(s) {}
  ~Socket() {
    if (value != INVALID_SOCKET)
      closesocket(value);
  }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
};
inline bool ready(SOCKET s, bool writing, Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval timeout{0, 100000};
    const int result = select(0, writing ? nullptr : &set,
                              writing ? &set : nullptr, nullptr, &timeout);
    if (result > 0)
      return true;
    if (result < 0)
      return false;
  }
  return false;
}
inline void timeouts(SOCKET s) {
  const DWORD ms = 100;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms),
             sizeof(ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms),
             sizeof(ms));
}
inline bool sendAll(SOCKET s, std::string_view bytes,
                    Clock::time_point deadline) {
  while (!bytes.empty() && Clock::now() < deadline) {
    if (!ready(s, true, deadline))
      return false;
    const auto n = send(s, bytes.data(),
                        int(std::min<std::size_t>(bytes.size(), 65536)), 0);
    if (n <= 0)
      return false;
    bytes.remove_prefix(std::size_t(n));
  }
  return bytes.empty();
}
inline std::string response(std::string_view body,
                            std::string_view type = "application/json",
                            unsigned status = 200) {
  return "HTTP/1.1 " + std::to_string(status) +
         (status == 200 ? " OK\r\n" : " Bad Request\r\n") +
         "Content-Type: " + std::string(type) +
         "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + std::string(body);
}
struct Capture {
  std::string raw, method, target, body;
  bool complete = false, matched = false;
};

struct Barrier {
  std::mutex mutex;
  std::condition_variable ready;
  bool arrived = false, released = false;
  bool waitReached() {
    std::unique_lock lock(mutex);
    return ready.wait_for(lock, std::chrono::seconds(2),
                          [&] { return arrived; });
  }
  void reach() {
    std::unique_lock lock(mutex);
    arrived = true;
    ready.notify_all();
    ready.wait_for(lock, std::chrono::seconds(2), [&] { return released; });
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    ready.notify_all();
  }
};
struct OwnedHostReply { bool matched=false; std::string wire; };
struct FixtureOptions {
  bool ipv6 = false, keepOpen = false;
  std::function<void()> accepted, beforeReply;
  std::function<void(unsigned)> afterSegment;
  std::vector<std::string> segments, replies;
  struct ExpectedRequest { std::string method, target, body, reply; };
  std::vector<ExpectedRequest> script; // At most 64 real sequential operations.
  bool fixedProtocol8189=false;
  std::function<OwnedHostReply(const Capture&)> ownedHostReply;
};
class NativeComfyFixture {
public:
  NativeComfyFixture(std::string method, std::string target, std::string body,
                     std::string reply, FixtureOptions setup = {})
      : expectedMethod(std::move(method)), expectedTarget(std::move(target)),
        expectedBody(std::move(body)), replyBytes(std::move(reply)),
        options(std::move(setup)),
        listener(socket(options.ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM,
                        IPPROTO_TCP)) {
    if (listener.value == INVALID_SOCKET || options.script.size() > 64
        || (options.ownedHostReply && !options.script.empty())
        || (options.fixedProtocol8189 && (options.ipv6 || !options.ownedHostReply)))
      return;
    if(options.fixedProtocol8189){const BOOL exclusive=TRUE;if(setsockopt(listener.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))!=0)return;}
    sockaddr_storage storage{};
    int length = options.ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    if (options.ipv6) {
      auto &address = reinterpret_cast<sockaddr_in6 &>(storage);
      address.sin6_family = AF_INET6;
      address.sin6_addr = in6addr_loopback;
    } else {
      auto &address = reinterpret_cast<sockaddr_in &>(storage);
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port=options.fixedProtocol8189?htons(8189):0;
    }
    if (bind(listener.value, reinterpret_cast<sockaddr *>(&storage), length) !=
            0 ||
        getsockname(listener.value, reinterpret_cast<sockaddr *>(&storage),
                    &length) != 0 ||
        listen(listener.value, 4) != 0)
      return;
    portValue =
        ntohs(options.ipv6 ? reinterpret_cast<sockaddr_in6 &>(storage).sin6_port
                           : reinterpret_cast<sockaddr_in &>(storage).sin_port);
    worker = std::thread([this] { run(); });
    started = true;
  }
  ~NativeComfyFixture() { join(); }
  NativeComfyFixture(const NativeComfyFixture &) = delete;
  bool valid() const noexcept { return started; }
  std::uint16_t port() const noexcept { return portValue; }
  unsigned requestCount() const noexcept { return connections.load(); }
  unsigned reusedBytes() const noexcept { return reused.load(); }
  std::vector<Capture> captures() const {
    std::lock_guard lock(mutex);
    return history;
  }
  bool exited() const noexcept { return done.load(); }
  void join() {
    stop.store(true);
    if (worker.joinable())
      worker.join();
  }
  bool rearmOwned() {
    if (!started || !options.fixedProtocol8189 || !options.ownedHostReply ||
        ownedPhases >= 4 || connections.load() >= 64)
      return false;
    join(); // The previous request/callback must actually leave before reuse.
    if (connections.load() >= 64)
      return false;
    stop.store(false);
    done.store(false);
    worker = std::thread([this] { run(); });
    ++ownedPhases;
    return true;
  }
  Capture captured() const {
    std::lock_guard lock(mutex);
    return capture;
  }

private:
  void run() noexcept {
    const auto deadline = Clock::now() + std::chrono::seconds(6);
    while (!stop.load() && Clock::now() < deadline && (!options.ownedHostReply || connections.load()<64)) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(listener.value, &set);
      timeval timeout{0, 100000};
      const int n = select(0, &set, nullptr, nullptr, &timeout);
      if (n < 0)
        break;
      if (!n)
        continue;
      Socket client(accept(listener.value, nullptr, nullptr));
      if (client.value == INVALID_SOCKET)
        break;
      const auto connection = connections.fetch_add(1);
      timeouts(client.value);
      if (options.accepted)
        options.accepted();
      Capture local;
      const auto requestDeadline = Clock::now() + std::chrono::seconds(2);
      std::size_t expectedLength = std::string::npos,
                  headerEnd = std::string::npos;
      while (!stop.load() && Clock::now() < requestDeadline &&
             local.raw.size() <= 49152) {
        if (!ready(client.value, false,
                   std::min(requestDeadline,
                            Clock::now() + std::chrono::milliseconds(100))))
          continue;
        std::array<char, 4096> bytes{};
        const int got = recv(client.value, bytes.data(), int(bytes.size()), 0);
        if (got <= 0)
          break;
        local.raw.append(bytes.data(), std::size_t(got));
        headerEnd = local.raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos &&
            expectedLength == std::string::npos) {
          expectedLength = 0;
          const auto key = local.raw.find("Content-Length: ");
          if (key != std::string::npos && key < headerEnd) {
            const auto start = key + 16;
            const auto end = local.raw.find("\r\n", start);
            if (end == std::string::npos || end - start > 8)
              break;
            for (auto i = start; i < end; ++i) {
              if (local.raw[i] < '0' || local.raw[i] > '9') {
                expectedLength = 65537;
                break;
              }
              expectedLength =
                  expectedLength * 10 + std::size_t(local.raw[i] - '0');
            }
          }
        }
        if (headerEnd != std::string::npos && expectedLength <= 16384 &&
            local.raw.size() >= headerEnd + 4 + expectedLength) {
          local.complete = local.raw.size() == headerEnd + 4 + expectedLength;
          break;
        }
      }
      const auto first = local.raw.find(' '),
                 second = local.raw.find(
                     ' ', first == std::string::npos ? 0 : first + 1);
      if (first != std::string::npos && second != std::string::npos) {
        local.method = local.raw.substr(0, first);
        local.target = local.raw.substr(first + 1, second - first - 1);
      }
      if (headerEnd != std::string::npos)
        local.body = local.raw.substr(headerEnd + 4);
      OwnedHostReply dynamic;
      if(options.ownedHostReply){
        if(local.complete&&local.raw.size()<=49152&&local.body.size()<=16384){try{dynamic=options.ownedHostReply(local);}catch(...){dynamic={};}}
        local.matched=dynamic.matched&&!dynamic.wire.empty()&&dynamic.wire.size()<=16u*1024*1024+4096;
      }else if (options.script.empty())
        local.matched = local.complete && local.method == expectedMethod &&
                        local.target == expectedTarget && local.body == expectedBody;
      else if (connection < options.script.size()) {
        const auto &expected = options.script[connection];
        local.matched = local.complete && local.method == expected.method &&
                        local.target == expected.target && local.body == expected.body;
      }
      {
        std::lock_guard lock(mutex);
        capture = local;
        history.push_back(local);
      }
      if (options.beforeReply)
        options.beforeReply();
      const auto reply = local.matched
                             ? (options.ownedHostReply ? std::move(dynamic.wire) : !options.script.empty() ? options.script[connection].reply
                                : connection < options.replies.size()
                                    ? options.replies[connection]
                                    : replyBytes)
                             : response("mismatch", "text/plain", 400);
      if (local.matched && !options.segments.empty()) {
        unsigned segment = 0;
        for (const auto &bytes : options.segments) {
          if (!sendAll(client.value, bytes, requestDeadline))
            break;
          if (options.afterSegment)
            options.afterSegment(segment);
          ++segment;
        }
      } else
        sendAll(client.value, reply, requestDeadline);
      if (options.keepOpen) {
        const auto end = Clock::now() + std::chrono::seconds(2);
        while (!stop.load() && Clock::now() < end) {
          if (!ready(
                  client.value, false,
                  std::min(end, Clock::now() + std::chrono::milliseconds(100))))
            continue;
          std::array<char, 4096> bytes{};
          const auto got =
              recv(client.value, bytes.data(), int(bytes.size()), 0);
          if (got <= 0)
            break;
          reused.fetch_add(unsigned(got));
        }
      }
      shutdown(client.value, SD_SEND);
      // Keep accepting until joined so an unexpected retry increments the
      // count.
    }
    done.store(true);
  }
  std::string expectedMethod, expectedTarget, expectedBody, replyBytes;
  FixtureOptions options;
  Socket listener;
  std::uint16_t portValue = 0;
  bool started = false;
  unsigned ownedPhases = 1; // Initial acceptance plus at most three re-arms.
  std::atomic_bool stop{false}, done{false};
  std::atomic_uint connections{0}, reused{0};
  mutable std::mutex mutex;
  Capture capture;
  std::vector<Capture> history;
  std::thread worker;
};

// Independent Winsock control: deliberately shares no production/WinHTTP code.
inline bool independentExchange(std::uint16_t port, const std::string &request,
                                std::string &result) {
  Socket client(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (client.value == INVALID_SOCKET)
    return false;
  u_long nonblocking = 1;
  if (ioctlsocket(client.value, FIONBIO, &nonblocking) != 0)
    return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  const int connected = connect(
      client.value, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  if (connected != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    return false;
  if (connected != 0) {
    if (!ready(client.value, true, deadline))
      return false;
    int error = 0, length = sizeof(error);
    if (getsockopt(client.value, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char *>(&error), &length) != 0 ||
        error)
      return false;
  }
  nonblocking = 0;
  if (ioctlsocket(client.value, FIONBIO, &nonblocking) != 0)
    return false;
  timeouts(client.value);
  if (!sendAll(client.value, request, deadline))
    return false;
  std::string candidate;
  while (Clock::now() < deadline && candidate.size() <= 4096) {
    if (!ready(client.value, false, deadline))
      return false;
    std::array<char, 1024> bytes{};
    const auto n = recv(client.value, bytes.data(), int(bytes.size()), 0);
    if (n == 0) {
      result = std::move(candidate);
      return true;
    }
    if (n < 0)
      return false;
    candidate.append(bytes.data(), std::size_t(n));
  }
  return false;
}
} // namespace comfy_fixture
