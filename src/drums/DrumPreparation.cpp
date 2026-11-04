#include "DrumPreparation.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#define FLAC__NO_DLL
#include <juce_audio_formats/codecs/flac/stream_decoder.h>
#undef FLAC__NO_DLL
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
#include <cstdio>
#endif
#include <limits>
#include <new>
#include <numbers>
#include <utility>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace airack::drums
{
namespace
{
bool fail(PreparationError value, PreparationError &error) noexcept
{
    error = value;
    return false;
}
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
using preparation_test::IoEvent;
using preparation_test::Phase;
using preparation_test::ReadMode;
thread_local preparation_test::Control *control = nullptr;
void progress(Phase phase, std::uint64_t n) noexcept
{
    if (control && control->progress)
        control->progress(phase, n, control->context);
}
void trace(preparation_test::ReadTrace *t, IoEvent e) noexcept
{
    if (!t)
        return;
    if (t->used == t->events.size())
    {
        t->valid = false;
        return;
    }
    t->events[t->used++] = e;
    if (t->pending && (e == IoEvent::releaseOverlapped || e == IoEvent::releaseEvent ||
                       e == IoEvent::releaseBuffer || e == IoEvent::closeSourceHandle))
        t->valid = false;
}
#else
enum class Phase
{
    preflight,
    hash,
    decode,
    resample,
    beforeCommit,
    readPending
};
void progress(Phase, std::uint64_t) noexcept
{
}
#endif
constexpr std::size_t readBytes = 65536;
enum class Codec
{
    wav,
    aiff,
    flac
};
struct Layout
{
    Codec codec = Codec::wav;
    std::uint64_t audioOffset = 0, audioBytes = 0;
    std::uint32_t rate = 0, channels = 0, bits = 0, minBlock = 0, maxBlock = 0;
    std::uint64_t frames = 0;
    std::array<std::uint8_t, 128> header{};
    std::size_t headerSize = 0;
    bool md5 = false;
};
struct SourceData
{
    HANDLE file = INVALID_HANDLE_VALUE, event = nullptr;
    std::unique_ptr<GeneratedDrumBytes> generated;
    std::unique_ptr<std::array<std::uint8_t, readBytes>> buffer;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::uint64_t bytes = 0;
    Layout layout;
    DrumSourceInfo info;
    std::optional<GeneratedOrigin> origin;
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
    preparation_test::ReadTrace *lifetimeTrace = nullptr;
#endif
    ~SourceData()
    {
        if (event)
        {
            CloseHandle(event);
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            trace(lifetimeTrace, IoEvent::releaseEvent);
#endif
        }
        buffer.reset();
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
        trace(lifetimeTrace, IoEvent::releaseBuffer);
#endif
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            trace(lifetimeTrace, IoEvent::closeSourceHandle);
#endif
        }
    }
};
struct Operation
{
    SourceData &source;
    const std::atomic_bool &cancel;
    PreparationError &error;
    GeneratedDrumBytes *borrowed = nullptr;
    bool good() noexcept
    {
        if (cancel.load(std::memory_order_relaxed))
            error = PreparationError::cancelled;
        return error == PreparationError::none;
    }
    bool latch(PreparationError e) noexcept
    {
        if (error == PreparationError::none)
            error = e;
        good();
        return false;
    }
    bool read(std::uint64_t offset, std::span<std::uint8_t> out, Phase phase) noexcept
    {
        if (!good())
            return false;
        progress(phase, offset);
        if (!good())
            return false;
        if (offset > source.bytes || out.size() > source.bytes - offset || out.size() > readBytes)
            return latch(PreparationError::ioError);
        if (out.empty())
            return true;
        if (auto *reader = borrowed ? borrowed : source.generated.get())
        {
            const auto scratch = std::span<std::byte>(
                reinterpret_cast<std::byte *>(source.buffer->data()), out.size());
            const bool read = reader->readAt(offset, scratch);
            if (!good())
                return false;
            if (!read)
                return latch(PreparationError::ioError);
            if (out.data() != source.buffer->data())
                std::memcpy(out.data(), source.buffer->data(), out.size());
            return true;
        }
        OVERLAPPED overlapped{};
        overlapped.Offset = DWORD(offset);
        overlapped.OffsetHigh = DWORD(offset >> 32);
        overlapped.hEvent = source.event;
        ResetEvent(source.event);
        DWORD requested = DWORD(out.size()), physicalRequest = requested, returned = 0,
              status = ERROR_SUCCESS;
        bool pending = false, cancelRequested = false, deadline = false;
        const auto started = GetTickCount64();
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
        const bool matched =
            control && control->readPhase == phase && control->readOffset == offset;
        const auto mode = matched ? control->readMode : ReadMode::system;
        auto *t = matched ? control->trace : nullptr;
        if (t)
        {
            source.lifetimeTrace = t;
            t->offset = offset;
            t->requestedBytes = requested;
            ++t->issued;
            trace(t, IoEvent::issue);
        }
        struct Release
        {
            preparation_test::ReadTrace *t;
            ~Release() { trace(t, IoEvent::releaseOverlapped); }
        } release{t};
        const bool scriptedPending =
            mode == ReadMode::pendingCancel || mode == ReadMode::pendingDeadline ||
            mode == ReadMode::pendingCompletionRace || mode == ReadMode::pendingWaitFailure;
        if (mode == ReadMode::shortSuccess)
            physicalRequest = requested - 1;
        BOOL issued = mode == ReadMode::readError
                          ? FALSE
                          : ReadFile(source.file, source.buffer->data(), physicalRequest, &returned,
                                     &overlapped);
        status = mode == ReadMode::readError ? ERROR_READ_FAULT
                 : issued                    ? ERROR_SUCCESS
                                             : GetLastError();
        if (scriptedPending && status == ERROR_SUCCESS)
            status = ERROR_IO_PENDING;
        unsigned scriptedPolls = 0;
#else
        const BOOL issued =
            ReadFile(source.file, source.buffer->data(), physicalRequest, &returned, &overlapped);
        status = issued ? ERROR_SUCCESS : GetLastError();
#endif
        pending = status == ERROR_IO_PENDING;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
        if (t && pending)
        {
            t->pending = true;
            trace(t, IoEvent::pending);
        }
#endif
        if (pending)
            progress(Phase::readPending, offset);
        while (pending)
        {
            auto elapsed = GetTickCount64() - started;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            if (mode == ReadMode::pendingDeadline)
                elapsed = 5001;
#endif
            if (!cancelRequested && (cancel.load(std::memory_order_relaxed) || elapsed >= 5000))
            {
                cancelRequested = true;
                deadline = !cancel.load(std::memory_order_relaxed);
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
                if (mode == ReadMode::pendingCompletionRace)
                    SetLastError(ERROR_NOT_FOUND);
                else
#endif
                    CancelIoEx(source.file, &overlapped);
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
                if (t)
                {
                    ++t->cancels;
                    trace(t, IoEvent::cancelRequest);
                }
#endif
            }
            DWORD wait = WAIT_OBJECT_0;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            if (mode == ReadMode::pendingWaitFailure)
                wait = WAIT_FAILED;
            if (!scriptedPending)
#endif
                wait = WaitForSingleObject(source.event, 25);
            if (wait == WAIT_FAILED)
            {
                if (!cancelRequested)
                {
                    cancelRequested = true;
                    deadline = true;
                    CancelIoEx(source.file, &overlapped);
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
                    if (t)
                    {
                        ++t->cancels;
                        trace(t, IoEvent::cancelRequest);
                    }
#endif
                }
                // A failed wait is not a completion verdict: keep polling and drain the read.
                Sleep(25);
            }
            if (wait == WAIT_TIMEOUT)
                continue;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            if (scriptedPending &&
                (!cancelRequested ||
                 scriptedPolls++ < (mode == ReadMode::pendingCompletionRace ? 1u : 2u)))
                continue;
#endif
            const BOOL complete = GetOverlappedResult(source.file, &overlapped, &returned, FALSE);
            status = complete ? ERROR_SUCCESS : GetLastError();
            if (status == ERROR_IO_INCOMPLETE)
                continue;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            if (scriptedPending && mode != ReadMode::pendingCompletionRace)
                status = ERROR_OPERATION_ABORTED;
#endif
            pending = false;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            if (t)
            {
                t->pending = false;
                ++t->completions;
                trace(t, IoEvent::completion);
                ++t->drains;
                trace(t, IoEvent::drained);
            }
#endif
        }
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
        if (t)
        {
            t->returnedBytes = returned;
            t->completionError = status;
            if (!t->completions && status == ERROR_SUCCESS)
            {
                ++t->completions;
                trace(t, IoEvent::completion);
            }
        }
#endif
        if (cancel.load(std::memory_order_relaxed))
            return latch(PreparationError::cancelled);
        if (deadline || cancelRequested || status != ERROR_SUCCESS || returned != requested)
            return latch(PreparationError::ioError);
        if (out.data() != source.buffer->data())
            std::memcpy(out.data(), source.buffer->data(), out.size());
        return true;
    }
};
std::uint64_t le(const std::uint8_t *p, unsigned n) noexcept
{
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= std::uint64_t(p[i]) << (i * 8);
    return v;
}
std::uint64_t be(const std::uint8_t *p, unsigned n) noexcept
{
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v = (v << 8) | p[i];
    return v;
}
void put(std::uint8_t *p, std::uint64_t x, unsigned n, bool big = false) noexcept
{
    for (unsigned i = 0; i < n; ++i)
        p[big ? n - 1 - i : i] = std::uint8_t(x >> (8 * i));
}
bool is(const std::uint8_t *p, const char *s) noexcept
{
    return std::memcmp(p, s, 4) == 0;
}
bool geometry(Operation &op) noexcept
{
    const auto &l = op.source.layout;
    if (l.rate < 8000 || l.rate > 384000 || (l.channels != 1 && l.channels != 2))
        return op.latch(PreparationError::unsupportedFormat);
    if (l.frames == 0)
        return op.latch(PreparationError::malformedSource);
    if (l.frames > std::uint64_t(l.rate) * (op.source.origin ? 101 : 6000) / 10)
        return op.latch(PreparationError::tooLarge);
    return true;
}
bool preflight(Operation &op) noexcept
{
    auto &l = op.source.layout;
    std::array<std::uint8_t, 64> first{};
    if (op.source.bytes < 4)
        return op.latch(PreparationError::malformedSource);
    if (!op.read(0,
                 std::span(first).first(std::size_t(std::min<std::uint64_t>(12, op.source.bytes))),
                 Phase::preflight))
        return false;
    if (is(first.data(), "fLaC"))
    {
        l.codec = Codec::flac;
        std::uint64_t pos = 4, metadata = 4;
        unsigned count = 0;
        bool stream = false, last = false;
        while (!last)
        {
            if (++count > 4096)
                return op.latch(PreparationError::tooLarge);
            if (pos > op.source.bytes || op.source.bytes - pos < 4)
                return op.latch(PreparationError::malformedSource);
            if (!op.read(pos, std::span(first).first(4), Phase::preflight))
                return false;
            last = (first[0] & 128) != 0;
            const auto type = first[0] & 127;
            const auto n = be(first.data() + 1, 3);
            pos += 4;
            if (n > op.source.bytes - pos)
                return op.latch(PreparationError::malformedSource);
            metadata += 4 + n;
            if (metadata > 1048576)
                return op.latch(PreparationError::tooLarge);
            if (!stream)
            {
                if (type != 0 || n != 34)
                    return op.latch(PreparationError::malformedSource);
                if (!op.read(pos, std::span(first).first(34), Phase::preflight))
                    return false;
                l.minBlock = std::uint32_t(be(first.data(), 2));
                l.maxBlock = std::uint32_t(be(first.data() + 2, 2));
                if (l.minBlock < 16 || l.maxBlock < l.minBlock || l.maxBlock > 16384)
                    return op.latch(PreparationError::unsupportedFormat);
                const auto packed = be(first.data() + 10, 8);
                l.rate = std::uint32_t(packed >> 44);
                l.channels = std::uint32_t((packed >> 41) & 7) + 1;
                l.bits = std::uint32_t((packed >> 36) & 31) + 1;
                l.frames = packed & 0xfffffffff;
                if (l.bits != 16 && l.bits != 24)
                    return op.latch(PreparationError::unsupportedFormat);
                l.md5 = std::any_of(first.begin() + 18, first.begin() + 34,
                                    [](auto x) { return x != 0; });
                std::memcpy(l.header.data(), "fLaC", 4);
                l.header[4] = 128;
                l.header[7] = 34;
                std::memcpy(l.header.data() + 8, first.data(), 34);
                l.headerSize = 42;
                stream = true;
            }
            else if (type == 0 || type == 127)
                return op.latch(PreparationError::malformedSource);
            pos += n;
        }
        l.audioOffset = pos;
        l.audioBytes = op.source.bytes - pos;
        if (!l.audioBytes)
            return op.latch(PreparationError::malformedSource);
        return geometry(op);
    }
    if (op.source.bytes < 12)
        return op.latch(PreparationError::malformedSource);
    const bool wav = is(first.data(), "RIFF") && is(first.data() + 8, "WAVE");
    const bool aiff =
        is(first.data(), "FORM") && (is(first.data() + 8, "AIFF") || is(first.data() + 8, "AIFC"));
    if (!wav && !aiff)
        return op.latch(PreparationError::unsupportedFormat);
    const bool aifc = aiff && is(first.data() + 8, "AIFC");
    l.codec = wav ? Codec::wav : Codec::aiff;
    if ((wav ? le(first.data() + 4, 4) : be(first.data() + 4, 4)) + 8 != op.source.bytes)
        return op.latch(PreparationError::malformedSource);
    std::array<std::uint8_t, 64> fmt{};
    std::size_t fmtSize = 0;
    std::uint64_t pos = 12, metadata = 12, ssndFrames = 0;
    unsigned chunks = 0;
    bool haveFormat = false, haveAudio = false, version = false;
    while (pos < op.source.bytes)
    {
        if (++chunks > 4096)
            return op.latch(PreparationError::tooLarge);
        if (op.source.bytes - pos < 8)
            return op.latch(PreparationError::malformedSource);
        if (!op.read(pos, std::span(first).first(8), Phase::preflight))
            return false;
        const auto n = wav ? le(first.data() + 4, 4) : be(first.data() + 4, 4);
        pos += 8;
        if (n > op.source.bytes - pos || (n & 1) > op.source.bytes - pos - n)
            return op.latch(PreparationError::malformedSource);
        metadata += 8 + (n & 1);
        if (is(first.data(), wav ? "fmt " : "COMM"))
        {
            if (haveFormat || n > (aifc ? 278u : 64u) || n < (wav ? 16u : aifc ? 23u : 18u))
                return op.latch(PreparationError::malformedSource);
            fmtSize = std::size_t(n);
            if (!op.read(pos, std::span(fmt).first(std::min(fmtSize, fmt.size())),
                         Phase::preflight))
                return false;
            haveFormat = true;
            metadata += n;
        }
        else if (is(first.data(), wav ? "data" : "SSND"))
        {
            if (haveAudio)
                return op.latch(PreparationError::malformedSource);
            haveAudio = true;
            l.audioOffset = pos;
            l.audioBytes = n;
            if (aiff)
            {
                if (n < 8 || !op.read(pos, std::span(first).first(8), Phase::preflight))
                    return op.latch(PreparationError::malformedSource);
                const auto offset = be(first.data(), 4);
                if (be(first.data() + 4, 4) != 0 || offset > n - 8)
                    return op.latch(PreparationError::malformedSource);
                l.audioOffset += 8 + offset;
                l.audioBytes -= 8 + offset;
                metadata += 8 + offset;
            }
        }
        else
        {
            metadata += n;
            if (aiff && is(first.data(), "FVER"))
            {
                if (version || n != 4 ||
                    !op.read(pos, std::span(first).first(4), Phase::preflight) ||
                    be(first.data(), 4) != 0xa2805140)
                    return op.latch(PreparationError::malformedSource);
                version = true;
            }
        }
        if (metadata > 1048576)
            return op.latch(PreparationError::tooLarge);
        pos += n + (n & 1);
    }
    if (!haveFormat || !haveAudio)
        return op.latch(PreparationError::malformedSource);
    if (wav)
    {
        auto tag = le(fmt.data(), 2);
        l.channels = std::uint32_t(le(fmt.data() + 2, 2));
        l.rate = std::uint32_t(le(fmt.data() + 4, 4));
        l.bits = std::uint32_t(le(fmt.data() + 14, 2));
        if (tag == 0xfffe)
        {
            static constexpr std::array<std::uint8_t, 12> suffix{0, 0,    0x10, 0,    0x80, 0,
                                                                 0, 0xaa, 0,    0x38, 0x9b, 0x71};
            if (fmtSize != 40 || le(fmt.data() + 16, 2) != 22 || le(fmt.data() + 18, 2) != l.bits ||
                !std::equal(suffix.begin(), suffix.end(), fmt.begin() + 28))
                return op.latch(PreparationError::malformedSource);
            const auto mask = le(fmt.data() + 20, 4);
            if (mask && mask != (l.channels == 1 ? 4u : 3u))
                return op.latch(PreparationError::unsupportedFormat);
            tag = le(fmt.data() + 24, 4);
        }
        else if (fmtSize != 16 && (fmtSize != 18 || le(fmt.data() + 16, 2) != 0))
            return op.latch(PreparationError::malformedSource);
        if ((tag != 1 && tag != 3) || (tag == 3 && l.bits != 32) ||
            (l.bits != 8 && l.bits != 16 && l.bits != 24 && l.bits != 32))
            return op.latch(PreparationError::unsupportedFormat);
        if (l.channels < 1 || l.channels > 2 || l.rate < 8000 || l.rate > 384000)
            return op.latch(PreparationError::unsupportedFormat);
        const auto align = l.channels * (l.bits / 8);
        if (le(fmt.data() + 12, 2) != align ||
            le(fmt.data() + 8, 4) != std::uint64_t(l.rate) * align || l.audioBytes % align)
            return op.latch(PreparationError::malformedSource);
        l.frames = l.audioBytes / align;
        std::memcpy(l.header.data(), "RIFF", 4);
        put(l.header.data() + 4, 20 + fmtSize + l.audioBytes + (l.audioBytes & 1), 4);
        std::memcpy(l.header.data() + 8, "WAVEfmt ", 8);
        put(l.header.data() + 16, fmtSize, 4);
        std::memcpy(l.header.data() + 20, fmt.data(), fmtSize);
        std::memcpy(l.header.data() + 20 + fmtSize, "data", 4);
        put(l.header.data() + 24 + fmtSize, l.audioBytes, 4);
        l.headerSize = 28 + fmtSize;
    }
    else
    {
        if ((aifc && !version) || (!aifc && fmtSize != 18))
            return op.latch(PreparationError::malformedSource);
        l.channels = std::uint32_t(be(fmt.data(), 2));
        ssndFrames = be(fmt.data() + 2, 4);
        l.bits = std::uint32_t(be(fmt.data() + 6, 2));
        const auto exponent = be(fmt.data() + 8, 2), mantissa = be(fmt.data() + 10, 8);
        if (exponent < 16383 || exponent > 16401 || (mantissa >> 63) != 1)
            return op.latch(PreparationError::unsupportedFormat);
        const auto shift = unsigned(16446 - exponent);
        if ((mantissa & ((std::uint64_t(1) << shift) - 1)) != 0)
            return op.latch(PreparationError::unsupportedFormat);
        l.rate = std::uint32_t(mantissa >> shift);
        if (l.bits != 8 && l.bits != 16 && l.bits != 24 && l.bits != 32)
            return op.latch(PreparationError::unsupportedFormat);
        if (aifc)
        {
            if (!is(fmt.data() + 18, "NONE") && !is(fmt.data() + 18, "twos") &&
                !is(fmt.data() + 18, "sowt") && !is(fmt.data() + 18, "fl32") &&
                !is(fmt.data() + 18, "FL32"))
                return op.latch(PreparationError::unsupportedFormat);
            if ((is(fmt.data() + 18, "fl32") || is(fmt.data() + 18, "FL32")) && l.bits != 32)
                return op.latch(PreparationError::unsupportedFormat);
            if (22u + ((std::size_t(fmt[22]) + 2u) & ~std::size_t(1)) != fmtSize)
                return op.latch(PreparationError::malformedSource);
        }
        if (l.channels < 1 || l.channels > 2 || l.rate < 8000 || l.rate > 384000)
            return op.latch(PreparationError::unsupportedFormat);
        const auto align = l.channels * (l.bits / 8);
        if (l.audioBytes % align || l.audioBytes / align != ssndFrames)
            return op.latch(PreparationError::malformedSource);
        l.frames = ssndFrames;
        auto *h = l.header.data();
        std::memcpy(h, "FORM", 4);
        std::memcpy(h + 8, aifc ? "AIFC" : "AIFF", 4);
        std::size_t p = 12;
        if (aifc)
        {
            std::memcpy(h + p, "FVER", 4);
            put(h + p + 4, 4, 4, true);
            put(h + p + 8, 0xa2805140, 4, true);
            p += 12;
        }
        std::memcpy(h + p, "COMM", 4);
        const auto size = aifc ? 24u : 18u;
        put(h + p + 4, size, 4, true);
        std::memcpy(h + p + 8, fmt.data(), aifc ? 22 : 18);
        p += 8 + size;
        std::memcpy(h + p, "SSND", 4);
        put(h + p + 4, l.audioBytes + 8, 4, true);
        l.headerSize = p + 16;
        put(h + 4, l.headerSize + l.audioBytes + (l.audioBytes & 1) - 8, 4, true);
    }
    return geometry(op);
}
class ContainerView final : public juce::InputStream
{
  public:
    explicit ContainerView(Operation &o) : op(o) {}
    juce::int64 getTotalLength() override
    {
        const auto &l = op.source.layout;
        return juce::int64(l.headerSize + l.audioBytes +
                           (l.codec == Codec::flac ? 0 : l.audioBytes & 1));
    }
    juce::int64 getPosition() override { return juce::int64(position); }
    bool setPosition(juce::int64 n) override
    {
        if (n < 0 || n > getTotalLength())
            return op.latch(PreparationError::ioError);
        position = std::uint64_t(n);
        return true;
    }
    bool isExhausted() override
    {
        return position == std::uint64_t(getTotalLength()) || !op.good();
    }
    int read(void *destination, int count) override
    {
        if (count <= 0 || !op.good())
            return 0;
        auto *dest = static_cast<std::uint8_t *>(destination);
        const auto &l = op.source.layout;
        const auto wanted = std::min<std::uint64_t>(std::uint64_t(count),
                                                    std::uint64_t(getTotalLength()) - position);
        std::uint64_t done = 0;
        while (done < wanted)
        {
            if (position < l.headerSize)
            {
                const auto n = std::min<std::uint64_t>(wanted - done, l.headerSize - position);
                std::memcpy(dest + done, l.header.data() + position, std::size_t(n));
                position += n;
                done += n;
            }
            else if (position - l.headerSize < l.audioBytes)
            {
                const auto offset = position - l.headerSize;
                const auto n =
                    std::min({wanted - done, l.audioBytes - offset, std::uint64_t(readBytes)});
                if (!op.read(l.audioOffset + offset, {dest + done, std::size_t(n)}, Phase::decode))
                    return int(done);
                position += n;
                done += n;
            }
            else
            {
                dest[done++] = 0;
                ++position;
            }
        }
        return int(done);
    }
    Operation &op;
    std::uint64_t position = 0;
};
class FileHashStream final : public juce::InputStream
{
  public:
    explicit FileHashStream(Operation &o) : op(o) {}
    juce::int64 getTotalLength() override { return juce::int64(op.source.bytes); }
    juce::int64 getPosition() override { return juce::int64(position); }
    bool setPosition(juce::int64) override { return false; }
    bool isExhausted() override { return position == op.source.bytes || !op.good(); }
    int read(void *out, int count) override
    {
        if (count <= 0 || !op.good())
            return 0;
        std::size_t done = 0;
        auto *d = static_cast<std::uint8_t *>(out);
        while (done < std::size_t(count) && position < op.source.bytes)
        {
            if (position == cachedEnd)
            {
                const auto n =
                    std::size_t(std::min<std::uint64_t>(readBytes, op.source.bytes - position));
                if (!op.read(position, std::span(*op.source.buffer).first(n), Phase::hash))
                    return int(done);
                cachedBegin = position;
                cachedEnd = position + n;
            }
            const auto n = std::min<std::uint64_t>(std::size_t(count) - done, cachedEnd - position);
            std::memcpy(d + done, op.source.buffer->data() + position - cachedBegin,
                        std::size_t(n));
            done += std::size_t(n);
            position += n;
        }
        return int(done);
    }

  private:
    Operation &op;
    std::uint64_t position = 0, cachedBegin = 0, cachedEnd = 0;
};
class Decoder
{
  public:
    explicit Decoder(Operation &o) : op(o), view(std::make_unique<ContainerView>(o)) {}
    bool open()
    {
        const auto &l = op.source.layout;
        if (l.codec != Codec::flac)
        {
            if (l.codec == Codec::wav)
            {
                juce::WavAudioFormat f;
                pcm.reset(f.createReaderFor(view.get(), false));
            }
            else
            {
                juce::AiffAudioFormat f;
                pcm.reset(f.createReaderFor(view.get(), false));
            }
            if (!pcm)
                return op.latch(PreparationError::malformedSource);
            view.release();
            if (pcm->sampleRate != l.rate || pcm->numChannels != l.channels ||
                pcm->bitsPerSample != l.bits || std::uint64_t(pcm->lengthInSamples) != l.frames)
                return op.latch(PreparationError::malformedSource);
            return op.good();
        }
        flac.reset(FLAC__stream_decoder_new());
        if (!flac)
            return op.latch(PreparationError::allocationFailed);
        if (!FLAC__stream_decoder_set_md5_checking(flac.get(), true))
            return op.latch(PreparationError::malformedSource);
        const auto status =
            FLAC__stream_decoder_init_stream(flac.get(), readFlac, nullptr, tellFlac, lengthFlac,
                                             eofFlac, writeFlac, metadataFlac, errorFlac, this);
        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
            return op.latch(status == FLAC__STREAM_DECODER_INIT_STATUS_MEMORY_ALLOCATION_ERROR
                                ? PreparationError::allocationFailed
                                : PreparationError::malformedSource);
        initialized = true;
        return true;
    }
    ~Decoder()
    {
        if (initialized)
            FLAC__stream_decoder_finish(flac.get());
    }
    std::size_t next(std::span<float> destination)
    {
        if (!op.good() || finished)
            return 0;
        const auto &l = op.source.layout;
        std::uint32_t partition = 4096;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
        if (control && (control->decodePartition == 1 || control->decodePartition == 7))
            partition = control->decodePartition;
#endif
        if (pcm)
        {
            if (consumed == l.frames)
            {
                finished = true;
                return 0;
            }
            const auto n = std::uint32_t(std::min<std::uint64_t>(partition, l.frames - consumed));
            float *channels[]{left.data(), right.data()};
            if (!pcm->read(channels, int(l.channels), juce::int64(consumed), int(n)))
                op.latch(PreparationError::malformedSource);
            if (!op.good())
                return 0;
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t ch = 0; ch < l.channels; ++ch)
                    destination[i * l.channels + ch] = channels[ch][i];
            consumed += n;
            progress(Phase::decode, consumed);
            return op.good() ? n : 0;
        }
        while (frameUsed == frameSize)
        {
            const auto previous = delivered;
            const auto ok = FLAC__stream_decoder_process_single(flac.get());
            if (!ok || !op.good() || bad)
                return failure();
            if (delivered != previous)
            {
                FLAC__uint64 end = 0;
                if (!FLAC__stream_decoder_get_decode_position(flac.get(), &end))
                    return failure();
                finalFrameEnd = end;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
                if (delivered == l.frames)
                    std::printf("FLAC_FRAME_END completed=%llu virtual_length=%llu samples=%llu\n",
                                static_cast<unsigned long long>(finalFrameEnd),
                                static_cast<unsigned long long>(view->getTotalLength()),
                                static_cast<unsigned long long>(delivered));
#endif
                // The completed CRC-checked frame end excludes unread decoder FIFO bytes.
                if (delivered == l.frames && finalFrameEnd != std::uint64_t(view->getTotalLength()))
                    return failure();
                break;
            }
            if (FLAC__stream_decoder_get_state(flac.get()) == FLAC__STREAM_DECODER_END_OF_STREAM)
            {
                const bool complete =
                    metadataSeen && delivered == l.frames && consumed == l.frames &&
                    finalFrameEnd == std::uint64_t(view->getTotalLength()) && !bad;
                const bool verified = FLAC__stream_decoder_finish(flac.get()) != 0;
                initialized = false;
                if (!complete || !verified || !op.good())
                    return failure();
                finished = true;
                md5Verified = l.md5;
                return 0;
            }
        }
        const auto n = std::min(partition, frameSize - frameUsed);
        const float scale = l.bits == 16 ? 0x1p-15f : 0x1p-23f;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t ch = 0; ch < l.channels; ++ch)
                destination[i * l.channels + ch] = float(frame[ch][frameUsed + i]) * scale;
        frameUsed += n;
        consumed += n;
        progress(Phase::decode, consumed);
        return op.good() ? n : 0;
    }
    bool complete() noexcept { return finished && op.good(); }
    bool md5Verified = false;
    std::uint64_t finalFrameEnd = 0, consumed = 0;

  private:
    std::size_t failure() noexcept
    {
        const auto state = FLAC__stream_decoder_get_state(flac.get());
        // Pinned native sequential decoding leaves MD5 workspace OOM as an unlatched abort.
        const bool md5AllocationFailed = state == FLAC__STREAM_DECODER_ABORTED && metadataSeen &&
                                         !bad && op.good();
        op.latch(state == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR || md5AllocationFailed
                     ? PreparationError::allocationFailed
                     : PreparationError::malformedSource);
        return 0;
    }
    static FLAC__StreamDecoderReadStatus readFlac(const FLAC__StreamDecoder *, FLAC__byte b[],
                                                  std::size_t *n, void *p) noexcept
    {
        auto &d = *static_cast<Decoder *>(p);
        if (!*n)
        {
            d.op.latch(PreparationError::malformedSource);
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        const int got = d.view->read(b, int(std::min<std::size_t>(*n, 65536)));
        *n = std::size_t(got);
        if (!d.op.good())
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        return got ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE
                   : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    static FLAC__StreamDecoderTellStatus tellFlac(const FLAC__StreamDecoder *, FLAC__uint64 *n,
                                                  void *p) noexcept
    {
        *n = std::uint64_t(static_cast<Decoder *>(p)->view->getPosition());
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }
    static FLAC__StreamDecoderLengthStatus lengthFlac(const FLAC__StreamDecoder *, FLAC__uint64 *n,
                                                      void *p) noexcept
    {
        *n = std::uint64_t(static_cast<Decoder *>(p)->view->getTotalLength());
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }
    static FLAC__bool eofFlac(const FLAC__StreamDecoder *, void *p) noexcept
    {
        return static_cast<Decoder *>(p)->view->isExhausted();
    }
    static void metadataFlac(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *m,
                             void *p) noexcept
    {
        auto &d = *static_cast<Decoder *>(p);
        const auto &l = d.op.source.layout;
        const auto &s = m->data.stream_info;
        if (d.metadataSeen || m->type != FLAC__METADATA_TYPE_STREAMINFO ||
            s.sample_rate != l.rate || s.channels != l.channels || s.bits_per_sample != l.bits ||
            s.total_samples != l.frames || s.min_blocksize != l.minBlock ||
            s.max_blocksize != l.maxBlock)
            d.bad = true;
        d.metadataSeen = true;
    }
    static void errorFlac(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus,
                          void *p) noexcept
    {
        static_cast<Decoder *>(p)->bad = true;
    }
    static FLAC__StreamDecoderWriteStatus writeFlac(const FLAC__StreamDecoder *,
                                                    const FLAC__Frame *f,
                                                    const FLAC__int32 *const b[], void *p) noexcept
    {
        auto &d = *static_cast<Decoder *>(p);
        const auto &l = d.op.source.layout;
        const auto &h = f->header;
        if (!d.metadataSeen || d.bad || h.sample_rate != l.rate || h.channels != l.channels ||
            h.bits_per_sample != l.bits || h.blocksize == 0 || h.blocksize > l.maxBlock ||
            h.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER ||
            h.number.sample_number != d.delivered || d.delivered > l.frames ||
            h.blocksize > l.frames - d.delivered)
        {
            d.bad = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        d.frameSize = h.blocksize;
        d.frameUsed = 0;
        for (unsigned ch = 0; ch < l.channels; ++ch)
            d.frame[ch] = b[ch];
        d.delivered += h.blocksize;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    Operation &op;
    std::unique_ptr<ContainerView> view;
    std::unique_ptr<juce::AudioFormatReader> pcm;
    std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)> flac{
        nullptr, &FLAC__stream_decoder_delete};
    std::array<float, 4096> left{}, right{};
    std::array<const FLAC__int32 *, 2> frame{};
    std::uint64_t delivered = 0;
    std::uint32_t frameUsed = 0, frameSize = 0;
    bool initialized = false, bad = false, metadataSeen = false, finished = false;
};
class DecodedHashStream final : public juce::InputStream
{
  public:
    DecodedHashStream(Operation &o, Decoder &d) : op(o), decoder(d) {}
    juce::int64 getTotalLength() override
    {
        return juce::int64(op.source.layout.frames * op.source.layout.channels * 4);
    }
    juce::int64 getPosition() override { return juce::int64(position); }
    bool setPosition(juce::int64) override { return false; }
    bool isExhausted() override { return decoder.complete() || !op.good(); }
    int read(void *out, int count) override
    {
        if (count <= 0 || !op.good())
            return 0;
        auto *dest = static_cast<std::uint8_t *>(out);
        std::size_t done = 0;
        while (done < std::size_t(count))
        {
            if (used == available)
            {
                const auto n = decoder.next(samples);
                if (!n)
                    return int(done);
                const auto &l = op.source.layout;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const auto frameIndex = frames + i;
                    const auto bucket = std::size_t(frameIndex * op.source.info.buckets / l.frames);
                    const bool first =
                        frameIndex ==
                        ((bucket * l.frames + op.source.info.buckets - 1) / op.source.info.buckets);
                    for (std::size_t ch = 0; ch < l.channels; ++ch)
                    {
                        const auto x = samples[i * l.channels + ch];
                        if (!std::isfinite(x))
                        {
                            op.latch(PreparationError::nonFinite);
                            return int(done);
                        }
                        op.source.info.peak = std::max(op.source.info.peak, std::abs(x));
                        auto &range = op.source.info.overview[ch][bucket];
                        if (first)
                            range = {x, x};
                        else
                        {
                            range.minimum = std::min(range.minimum, x);
                            range.maximum = std::max(range.maximum, x);
                        }
                        put(bytes.data() + (i * l.channels + ch) * 4,
                            std::bit_cast<std::uint32_t>(x), 4);
                    }
                }
                frames += n;
                used = 0;
                available = n * l.channels * 4;
            }
            const auto n = std::min(std::size_t(count) - done, available - used);
            std::memcpy(dest + done, bytes.data() + used, n);
            used += n;
            done += n;
            position += n;
        }
        return int(done);
    }

  private:
    Operation &op;
    Decoder &decoder;
    std::array<float, 8192> samples{};
    std::array<std::uint8_t, 32768> bytes{};
    std::size_t used = 0, available = 0;
    std::uint64_t position = 0, frames = 0;
};
AssetId sha(juce::InputStream &input)
{
    const auto raw = juce::SHA256(input).getRawData();
    AssetId id{};
    std::memcpy(id.data(), raw.getData(), 32);
    return id;
}
bool textValid(const std::string &s, std::size_t cap) noexcept
{
    return !s.empty() && s.size() <= cap &&
           std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; }) &&
           juce::CharPointer_UTF8::isValidString(s.data(), int(s.size()));
}
struct Prototype
{
    std::array<double, 49153> values{};
    static double i0(double x) noexcept
    {
        double sum = 1, term = 1;
        for (unsigned n = 1; n < 60; ++n)
        {
            term *= x * x / (4. * n * n);
            sum += term;
            if (term < sum * 1e-17)
                break;
        }
        return sum;
    }
    Prototype()
    {
        const auto denominator = i0(7.5);
        for (std::size_t i = 0; i < values.size() - 1; ++i)
        {
            const auto u = double(i) / 1024, z = .95 * u;
            values[i] = .95 * (i ? std::sin(std::numbers::pi * z) / (std::numbers::pi * z) : 1) *
                        i0(7.5 * std::sqrt(1 - u * u / (48 * 48))) / denominator;
        }
    }
    double at(double u) const noexcept
    {
        u = std::abs(u) * 1024;
        if (u >= 49152)
            return 0;
        const auto index = std::size_t(u);
        return values[index] + (values[index + 1] - values[index]) * (u - double(index));
    }
};
const Prototype &prototype()
{
    static const Prototype table;
    return table;
}
struct Resampler
{
    Operation &op;
    std::vector<float> &output;
    std::uint64_t begin, end;
    float gain;
    std::uint64_t received = 0, next = 0;
    std::array<float, 16384> ring{};
    std::array<float, 2> first{}, last{};
    float peak = 0;
    bool push(std::span<const float> block, std::uint64_t start, std::size_t count)
    {
        const auto channels = op.source.layout.channels;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto index = start + i;
            if (index < begin || index >= end)
                continue;
            for (unsigned ch = 0; ch < channels; ++ch)
            {
                const auto x = block[i * channels + ch];
                if (!std::isfinite(x))
                    return op.latch(PreparationError::nonFinite);
                ring[(index % 8192) * channels + ch] = x;
                if (index == begin)
                    first[ch] = x;
                last[ch] = x;
            }
            received = index + 1;
        }
        return produce(false);
    }
    float sample(std::int64_t frame, unsigned ch) const noexcept
    {
        if (frame < std::int64_t(begin))
            return first[ch];
        if (frame >= std::int64_t(end))
            return last[ch];
        return ring[(std::uint64_t(frame) % 8192) * op.source.layout.channels + ch];
    }
    bool produce(bool final)
    {
        const auto &l = op.source.layout;
        const auto frames = output.size() / l.channels;
        const double m = std::max(1., l.rate / 48000.), span = 48 * m;
        while (next < frames)
        {
            const auto rational = next * l.rate;
            const auto whole = begin + rational / 48000;
            const double x = double(whole) + double(rational % 48000) / 48000;
            if (!final && received < end &&
                (l.rate == 48000 ? whole >= received : std::floor(x + span) >= double(received)))
                break;
            if (next % 64 == 0)
            {
                progress(Phase::resample, next);
                if (!op.good())
                    return false;
            }
            std::array<double, 2> sum{};
            double weight = 0;
            if (l.rate == 48000)
            {
                for (unsigned ch = 0; ch < l.channels; ++ch)
                {
                    const float input = sample(std::int64_t(whole), ch);
                    output[next * l.channels + ch] =
                        gain == 1 ? input : float(double(input) * double(gain));
                }
            }
            else
            {
                const auto &table = prototype();
                const auto low = std::int64_t(std::ceil(x - span)),
                           high = std::int64_t(std::floor(x + span));
                for (auto j = low; j <= high; ++j)
                {
                    const auto w = table.at((double(j) - x) / m);
                    weight += w;
                    for (unsigned ch = 0; ch < l.channels; ++ch)
                        sum[ch] += w * sample(j, ch);
                }
                for (unsigned ch = 0; ch < l.channels; ++ch)
                {
                    const auto value = (sum[ch] / weight) * double(gain);
                    if (!std::isfinite(value))
                        return op.latch(PreparationError::nonFinite);
                    output[next * l.channels + ch] = float(value);
                }
            }
            for (unsigned ch = 0; ch < l.channels; ++ch)
            {
                const auto value = output[next * l.channels + ch];
                if (!std::isfinite(value))
                    return op.latch(PreparationError::nonFinite);
                peak = std::max(peak, std::abs(value));
                if (peak > 1)
                    return op.latch(PreparationError::gainRequired);
            }
            ++next;
        }
        return op.good();
    }
};
bool inspectValidated(SourceData &source, const std::atomic_bool &cancel,
                      PreparationError &error, GeneratedDrumBytes *borrowed = nullptr)
{
        const auto &origin = source.origin;
        Operation op{source, cancel, error, borrowed};
        if (!preflight(op))
            return fail(op.error, error);
        const auto &l = source.layout;
        if (origin && (l.codec != Codec::flac || l.rate != 44100 || l.channels != 2 ||
                       l.bits != 16 || l.frames != 442368 || !l.md5))
            return fail(PreparationError::identityMismatch, error);
        source.info.rate = l.rate;
        source.info.channels = l.channels;
        source.info.bits = l.bits;
        source.info.frames = l.frames;
        source.info.encodedBytes = source.bytes;
        source.info.buckets = std::uint32_t(std::min<std::uint64_t>(512, l.frames));
        FileHashStream original(op);
        source.info.fileId = sha(original);
        if (!op.good() || std::uint64_t(original.getPosition()) != source.bytes)
            return fail(op.error == PreparationError::none ? PreparationError::ioError : op.error,
                        error);
        if (origin && source.info.fileId != origin->expectedFileId)
            return fail(PreparationError::identityMismatch, error);
        {
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            if (control)
                ++control->decoderConstructions;
#endif
            auto decoder = std::make_unique<Decoder>(op);
            if (!decoder->open())
                return fail(op.error, error);
            auto decoded = std::make_unique<DecodedHashStream>(op, *decoder);
            source.info.decodedPcmId = sha(*decoded);
            if (!decoder->complete() || !op.good())
                return fail(op.error == PreparationError::none ? PreparationError::malformedSource
                                                               : op.error,
                            error);
            source.info.md5Verified = decoder->md5Verified;
        }
        progress(Phase::beforeCommit, source.bytes);
        if (!op.good())
            return fail(op.error, error);
    return true;
}
} // namespace
struct DrumSource::Impl : SourceData
{
};
DrumSource::DrumSource(std::unique_ptr<Impl> p) noexcept : impl_(std::move(p))
{
}
DrumSource::~DrumSource() = default;
DrumSource::DrumSource(DrumSource &&) noexcept = default;
DrumSource &DrumSource::operator=(DrumSource &&) noexcept = default;
const DrumSourceInfo &DrumSource::info() const noexcept
{
    return impl_->info;
}
bool inspectDrumSource(const juce::File &file, const std::optional<GeneratedOrigin> &origin,
                       const std::atomic_bool &cancel, std::unique_ptr<DrumSource> &destination,
                       PreparationError &error) noexcept
{
    error = PreparationError::none;
    if (cancel.load(std::memory_order_relaxed))
        return fail(PreparationError::cancelled, error);
    if (origin && (!textValid(origin->generator, 96) || !textValid(origin->condition, 128) ||
                   origin->expectedFileId == AssetId{} || origin->promptId == AssetId{}))
        return fail(PreparationError::identityMismatch, error);
    try
    {
        auto source = std::make_unique<DrumSource::Impl>();
        source->origin = origin;
        source->file = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_READ,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (source->file == INVALID_HANDLE_VALUE)
            return fail(GetLastError() == ERROR_SHARING_VIOLATION ? PreparationError::sourceBusy
                                                                  : PreparationError::ioError,
                        error);
        LARGE_INTEGER size{};
        if (GetFileType(source->file) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(source->file, &source->identity) ||
            !GetFileSizeEx(source->file, &size) ||
            (source->identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || size.QuadPart < 0)
            return fail(PreparationError::ioError, error);
        if (size.QuadPart == 0)
            return fail(PreparationError::malformedSource, error);
        source->bytes = std::uint64_t(size.QuadPart);
        if (source->bytes > (origin ? 16ull : 256ull) * 1024 * 1024)
            return fail(PreparationError::tooLarge, error);
        source->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!source->event)
            return fail(PreparationError::ioError, error);
        source->buffer = std::make_unique<std::array<std::uint8_t, readBytes>>();
        if (!inspectValidated(*source, cancel, error))
            return false;
        auto result = std::unique_ptr<DrumSource>(new DrumSource(std::move(source)));
        destination.swap(result);
        error = PreparationError::none;
        return true;
    }
    catch (const std::bad_alloc &)
    {
        return fail(cancel.load()                        ? PreparationError::cancelled
                    : error == PreparationError::ioError ? PreparationError::ioError
                                                         : PreparationError::allocationFailed,
                    error);
    }
}
bool inspectDrumSource(std::unique_ptr<GeneratedDrumBytes> &input, const GeneratedOrigin &origin,
                       const std::atomic_bool &cancel, std::unique_ptr<DrumSource> &destination,
                       PreparationError &error) noexcept
{
    error = PreparationError::none;
    if (cancel.load(std::memory_order_relaxed))
        return fail(PreparationError::cancelled, error);
    if (!input || !textValid(origin.generator, 96) || !textValid(origin.condition, 128) ||
        origin.expectedFileId == AssetId{} || origin.promptId == AssetId{})
        return fail(PreparationError::identityMismatch, error);
    try
    {
        auto shell = std::unique_ptr<DrumSource>(new DrumSource(std::make_unique<DrumSource::Impl>()));
        auto &source = *shell->impl_;
        source.origin = origin;
        source.bytes = input->size();
        if (!source.bytes)
            return fail(PreparationError::malformedSource, error);
        if (source.bytes > 16ull * 1024 * 1024)
            return fail(PreparationError::tooLarge, error);
        source.buffer = std::make_unique<std::array<std::uint8_t, readBytes>>();
        if (!inspectValidated(source, cancel, error, input.get()))
            return false;
        source.generated = std::move(input);
        destination.swap(shell);
        return true;
    }
    catch (const std::bad_alloc &)
    {
        return fail(cancel.load() ? PreparationError::cancelled : PreparationError::allocationFailed, error);
    }
}
bool prepareDrumSample(DrumSource &held, std::uint64_t begin, std::uint64_t end, float gain,
                       const std::atomic_bool &cancel, PreparedDrumSample &destination,
                       PreparationError &error) noexcept
{
    error = PreparationError::none;
    if (cancel.load(std::memory_order_relaxed))
        return fail(PreparationError::cancelled, error);
    auto &source = *held.impl_;
    if (source.busy.test_and_set(std::memory_order_acquire))
        return fail(PreparationError::sourceBusy, error);
    struct Release
    {
        SourceData &source;
        ~Release() { source.busy.clear(std::memory_order_release); }
    } release{source};
    const auto &l = source.layout;
    if (begin >= end || end > l.frames || end - begin > std::uint64_t(l.rate) * 10)
        return fail(PreparationError::invalidSelection, error);
    if (!std::isfinite(gain) || gain <= 0 || gain > 16)
        return fail(PreparationError::invalidGain, error);
    const auto frames = ((end - begin) * 48000 + l.rate - 1) / l.rate;
    if (!frames || frames > 480000)
        return fail(PreparationError::invalidSelection, error);
    try
    {
        Operation op{source, cancel, error};
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
        if (control)
            ++control->canonicalAllocations;
#endif
        std::vector<float> canonical(std::size_t(frames) * l.channels);
        float peak = 0;
        {
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
            if (control)
                ++control->decoderConstructions;
#endif
            auto decoder = std::make_unique<Decoder>(op);
            if (!decoder->open())
                return fail(op.error, error);
            auto converter = std::make_unique<Resampler>(op, canonical, begin, end, gain);
            std::array<float, 8192> block{};
            std::uint64_t position = 0;
            for (;;)
            {
                const auto n = decoder->next(block);
                if (!n)
                    break;
                if (!converter->push(block, position, n))
                    return fail(op.error, error);
                position += n;
            }
            if (!decoder->complete() || !op.good())
                return fail(op.error == PreparationError::none ? PreparationError::malformedSource
                                                               : op.error,
                            error);
            if (!converter->produce(true) || converter->next != frames)
                return fail(op.error == PreparationError::none ? PreparationError::malformedSource
                                                               : op.error,
                            error);
            peak = converter->peak;
        }
        PreparedDrumSample result;
        AssetError assetError{};
        if (!makeSampleAsset(canonical, l.channels, result.asset, assetError))
        {
            auto mapped = PreparationError::invalidSelection;
            if (assetError == AssetError::allocationFailed)
                mapped = PreparationError::allocationFailed;
            else if (assetError == AssetError::nonFinite)
                mapped = PreparationError::nonFinite;
            else if (assetError == AssetError::outOfRange)
                mapped = PreparationError::gainRequired;
            return fail(mapped, error);
        }
        auto &p = result.source;
        p.sourceFileId = source.info.fileId;
        p.sourceRate = l.rate;
        p.sourceChannels = l.channels;
        p.sourceFrames = l.frames;
        p.begin = begin;
        p.end = end;
        p.gain = gain;
        if (source.origin)
        {
            p.kind = SourceKind::generated;
            p.generator = source.origin->generator;
            p.condition = source.origin->condition;
            p.seed = source.origin->seed;
            p.promptId = source.origin->promptId;
        }
        result.peak = peak;
        progress(Phase::beforeCommit, frames);
        if (!op.good())
            return fail(op.error, error);
        std::swap(destination, result);
        error = PreparationError::none;
        return true;
    }
    catch (const std::bad_alloc &)
    {
        return fail(cancel.load()                        ? PreparationError::cancelled
                    : error == PreparationError::ioError ? PreparationError::ioError
                                                         : PreparationError::allocationFailed,
                    error);
    }
}
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
namespace preparation_test
{
void useControl(Control *c) noexcept
{
    control = c;
}
bool Access::readAt(DrumSource &s, std::uint64_t offset, std::span<std::uint8_t> dest,
                    const std::atomic_bool &cancel, PreparationError &error) noexcept
{
    error = PreparationError::none;
    Operation op{*s.impl_, cancel, error};
    const auto ok = op.read(offset, dest, Phase::decode);
    error = op.error;
    return ok;
}
bool Access::viewPosition(DrumSource &s, std::uint64_t position, PreparationError &error) noexcept
{
    std::atomic_bool cancel{false};
    error = PreparationError::none;
    Operation op{*s.impl_, cancel, error};
    ContainerView view(op);
    const auto ok = position <= INT64_MAX && view.setPosition(juce::int64(position));
    if (!ok && op.error == PreparationError::none)
        op.error = PreparationError::ioError;
    error = op.error;
    return ok;
}
} // namespace preparation_test
#endif
} // namespace airack::drums
