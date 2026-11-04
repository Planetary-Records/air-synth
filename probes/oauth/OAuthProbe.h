#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace rack::oauth
{
enum class Status { ready, signedOut, awaitingCallback, connected, denied, cancelled,
    invalidCallback, expiredAttempt, tokenFailure, invalidIdentity, identityMismatch,
    inferenceNotGranted, networkFailure, invalidResponse, accessExpired,
    storageFailure, busy };
const char* statusName(Status);
struct Candidate { juce::String engine; double cutoffHz {}; };
struct Model { juce::String slug, displayName; };
struct Result
{
    Status status { Status::ready };
    std::optional<Candidate> candidate;
    std::vector<Model> models;
    int httpStatus {};
    juce::String errorCode, requestId;
};
inline constexpr std::size_t maximumStructuredPromptBytes = 1024 * 1024;
inline constexpr std::size_t maximumStructuredSchemaBytes = 64 * 1024;
inline constexpr std::size_t maximumStructuredRequestBytes = 2 * 1024 * 1024;
inline constexpr std::size_t maximumStructuredOutputBytes = 262144;
struct StructuredResult { Result result; juce::String text; };
// These transport records are internal credential-bearing inputs, never diagnostics.
struct HttpRequest { juce::String method, url, body, bearer, contentType; size_t maxBytes {}; int timeoutMs {}; };
enum class HttpFailure { none, open, configuration, connect, request, requestHeaders,
    send, receive, responseHeaders, read, bodyLimit, bodyEncoding, cleanup, cancelled, deadline };
struct HttpResponse { int status {}; juce::String body, requestId; bool interrupted {};
    unsigned nativeError {}; std::uint64_t receivedBytes {}; HttpFailure failure {HttpFailure::none}; };
using Transport = std::function<HttpResponse(const HttpRequest&, const std::atomic_bool&)>;
using Clock = std::function<int64_t()>;
struct SignIn { Result result; juce::String browserUrl; };
enum class SignInMode { existingAccount, addAccount };
inline constexpr std::size_t maximumSavedAccounts = 16;
struct SavedAccount { juce::String id, label; bool operator==(const SavedAccount&) const = default; };

// Call methods serially on a worker thread. Only cancel() is cross-thread safe.
class OAuthProbe
{
public:
    OAuthProbe(juce::File storeDirectory, Transport = {}, Clock = {});
    ~OAuthProbe();
    SignIn beginSignIn(int loopbackPort, SignInMode = SignInMode::existingAccount, const juce::String& savedAccount = {});
    Result acceptCallback(const juce::String& requestTarget);
    void cancel();
    Result status();
    Result savedAccounts(std::vector<SavedAccount>&, juce::String& active);
    Result selectAccount(const juce::String& id);
    Result listModels();
    Result requestStructuredCandidate(const juce::String& model);
    StructuredResult requestStructured(const juce::String& model,
        std::string_view utf8Prompt, const juce::var& schema);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

namespace detail
{
juce::var strictJson(const juce::String&);
std::optional<Candidate> parseCandidate(const juce::String&);
Result parseEventStream(const juce::String&);
juce::String streamErrorDiagnostic(const juce::String&);
StructuredResult parseStructuredEventStream(const juce::String&,
    std::size_t maximumTextBytes = maximumStructuredOutputBytes);
bool nativeBodySizeAllowed(const HttpRequest&) noexcept;
Result parseModels(const juce::String&);
juce::String randomUrlToken(size_t bytes);
juce::String sha256Url(const juce::String&);
juce::String base64Url(const void*, size_t);
bool verifyIdentity(const juce::String& jwt, const juce::String& jwks,
                    const juce::String& client, const juce::String& nonce,
                    int64_t now, juce::String& subject);
HttpResponse nativeHttp(const HttpRequest&, const std::atomic_bool&);
// Native request ownership, shared by the caller, cleanup owner and final callback.
// Close is injectable so the exceptional Windows lifecycle can be checked offline.
class NativeHttpLifecycle : public std::enable_shared_from_this<NativeHttpLifecycle>
{
public:
    enum class Event { none, sent, headers, read, error, closing };
    using Deadline = std::chrono::steady_clock::time_point;
    using Close = std::function<bool(void*)>;
    static std::shared_ptr<NativeHttpLifecycle> create(std::array<void*,3> handles,
        juce::String sendBody, Close, std::shared_ptr<std::atomic_bool> available);
    ~NativeHttpLifecycle();
    void expectCallbacks();
    void notify(Event, size_t bytes = 0);
    void notifyRequestError(const void* statusInfo, size_t length);
    unsigned nativeError();
    void reset();
    bool wait(Event, const std::atomic_bool&, Deadline);
    bool finish(const std::atomic_bool&, Deadline);
    const juce::String& sendBody() const { return body; }
    char* readBuffer() { return buffer.data(); }
    size_t bytesRead();
private:
    NativeHttpLifecycle(std::array<void*,3>,juce::String,Close,std::shared_ptr<std::atomic_bool>);
    void retireAdmission(); // Called with mutex held, only after complete safe cleanup.
    std::mutex mutex;
    std::condition_variable changed;
    Event event {Event::none};
    size_t bytes {};
    unsigned nativeFailure {};
    bool failed {}, closed {true}, cleanupStarted {}, cleanupDone {}, closeSucceeded {};
    std::array<void*,3> handles;
    juce::String body;
    std::array<char,8192> buffer {};
    Close close;
    std::shared_ptr<std::atomic_bool> available;
    std::shared_ptr<NativeHttpLifecycle> retained;
};
bool nativeHttpTimeouts(void* handle, const HttpRequest&, NativeHttpLifecycle::Deadline);
class Store
{
public:
    explicit Store(juce::File directory);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    bool lock(const std::atomic_bool&);
    bool load(juce::var&);
    bool save(const juce::var&);
private:
    juce::File directory;
    void* lockHandle {};
};
class Loopback
{
public:
    Loopback();
    ~Loopback();
    int port() const;
    juce::String receive(const std::atomic_bool&, int timeoutMs = 180000);
private:
    juce::StreamingSocket socket;
};
}
}
