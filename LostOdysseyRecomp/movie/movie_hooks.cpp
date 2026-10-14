// Native cutscene video. The game's WMV player (the Xbox Windows Media SDK,
// run as recompiled guest code) decodes 720p WMV3 slower than real time on the
// Switch, and its GetNextFrame blocks the render thread until a frame is
// ready, so cutscenes stutter and the whole game stalls. With this on, the
// guest player is created audio-only (video track -1, a mode it supports) and
// the video is decoded on a host thread with FFmpeg's VC-1 decoder; frames go
// into the same Y/U/V textures the guest player would have filled. The guest
// player still plays the audio; the frame hook starts and steps it the way the
// stock GetNextFrame does.
//
// Guest functions involved (see docs/notes or the hooks below):
//   sub_82815060(M)        create the player for movie M (M+20 player, M+44 info)
//   sub_82CE9728(f, p, o)  player factory; p+24 audio track, p+28 video track
//   sub_82CE9100(pl, out)  video size: out[0] width, out[1] height
//   sub_823F0FA8(pl, d)    next frame into descriptor d (render thread)
//   sub_82CE9248 / sub_82CE9FD8(pl)  pause / resume
// Set LO_NATIVE_MOVIES=0 to use the guest decoder.
#include <stdafx.h>

#if LO_HAS_WMV3_DECODER

#include <os/logger.h>
#include <os/thread_name.h>
#include <kernel/io/file_system.h>
#include <host_ui/host_ui.h>
#include <modding/fpi_index.h>
#include "wmv_stream.h"
#if LO_PLATFORM_SWITCH
#include <os/switch_platform.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

extern "C" PPC_FUNC(__imp__sub_82815060);
extern "C" PPC_FUNC(__imp__sub_82CE9728);
extern "C" PPC_FUNC(__imp__sub_82CE9100);
extern "C" PPC_FUNC(__imp__sub_823F02A8);
extern "C" PPC_FUNC(__imp__sub_823F0FA8);
extern "C" PPC_FUNC(__imp__sub_82CE9248);
extern "C" PPC_FUNC(__imp__sub_82CE9FD8);

namespace
{
namespace fs = std::filesystem;

// GetNextFrame results the movie update (sub_823F0038) checks for.
constexpr uint32_t kEndOfMovie = 0x16660026;
constexpr uint32_t kNoNewFrame = 0x166600E8;

bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("LO_NATIVE_MOVIES");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

std::string Lower(std::string text)
{
    for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// One movie at a time: the game plays a single cutscene and starts each one
// through sub_82815060, which replaces the previous one here.
struct Movie
{
    std::unique_ptr<movie::WmvStream> stream;
    std::string name;
    uint32_t player = 0;      // guest player object (M+20)
    bool started = false;     // first frame requested
    uint64_t startNs = 0;     // game time of the first request
    uint64_t pausedNs = 0, pauseStartNs = 0;
    bool paused = false;
    uint32_t shown = 0, late = 0;
    uint32_t lastPts = 0;
    bool loggedEnd = false;
    bool guestStarted = false, startFailed = false, ended = false;
};

std::mutex g_mutex;
std::unique_ptr<Movie> g_movie;
// Set while sub_82815060 runs for a movie that will be decoded on the host.
thread_local Movie* t_pending = nullptr;

// Archive and offset of a movie file on the current disc, from its LO.fpi.
struct Location
{
    std::string archive;
    uint64_t offset = 0;
    uint64_t size = 0;
};

bool Locate(const std::string& name, Location& location, std::string& error)
{
    const fs::path index = FileSystem::ResolvePath("\\Device\\Cdrom0\\LO.fpi");
    if (index.empty())
    {
        error = "no disc";
        return false;
    }
    // Parsed once per disc.
    static std::mutex cacheMutex;
    std::lock_guard cacheLock(cacheMutex);
    static std::map<std::string, modding::fpi::Index> cache;
    const std::string key = FileSystem::PathUtf8(index);
    auto it = cache.find(key);
    if (it == cache.end())
    {
        std::ifstream input(index, std::ios::binary);
        std::vector<uint8_t> bytes(64);
        if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), 64))
        {
            error = "cannot read " + key;
            return false;
        }
        try
        {
            const auto used = modding::fpi::IndexSize(bytes);
            bytes.resize(size_t(used));
            if (!input.read(reinterpret_cast<char*>(bytes.data()) + 64, std::streamsize(used - 64)))
            {
                error = "short read of " + key;
                return false;
            }
            it = cache.emplace(key, modding::fpi::Read(bytes)).first;
        }
        catch (const std::exception& e)
        {
            error = std::string("bad index: ") + e.what();
            return false;
        }
    }
    const auto& parsed = it->second;
    // The game names movies by file name ("mv_00v.wmv"); the index holds
    // "bin/xenon/mov/mv_00v.wmv".
    const std::string suffix = "/" + name;
    const modding::fpi::File* found = nullptr;
    for (const auto& file : parsed.files)
    {
        if (file.path == name || (file.path.size() > suffix.size() &&
                                  file.path.compare(file.path.size() - suffix.size(), suffix.size(), suffix) == 0))
        {
            // Prefer the movie folder over test data with the same name.
            if (!found || file.path.find("/mov/") != std::string::npos) found = &file;
        }
    }
    if (!found)
    {
        error = name + " is not in the disc index";
        return false;
    }
    if (found->archive >= parsed.archives.size())
    {
        error = "bad archive number";
        return false;
    }
    // Archive names in the index are lower case; match the disc files case-insensitively.
    const std::string archiveName = parsed.archives[found->archive];
    fs::path archive;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(index.parent_path(), ec))
    {
        if (Lower(FileSystem::PathUtf8(entry.path().filename())) == archiveName)
        {
            archive = entry.path();
            break;
        }
    }
    if (archive.empty())
    {
        error = archiveName + " missing";
        return false;
    }
    location.archive = FileSystem::PathUtf8(archive);
    location.offset = found->offset;
    location.size = found->size;
    return true;
}

// The movie's name: an FString (UTF-16 big endian) at info+48.
std::string MovieName(uint8_t* base, uint32_t info)
{
    const uint32_t data = PPC_LOAD_U32(info + 48);
    const int32_t count = int32_t(PPC_LOAD_U32(info + 52));
    std::string name;
    if (!data || count <= 0 || count > 260) return name;
    for (int32_t i = 0; i < count; ++i)
    {
        const uint16_t c = PPC_LOAD_U16(data + uint32_t(i) * 2);
        if (!c) break;
        if (c >= 0x80) return {};
        name += char(c == '\\' ? '/' : std::tolower(c));
    }
    if (const auto slash = name.rfind('/'); slash != std::string::npos) name.erase(0, slash + 1);
    return name;
}

void DecoderThreadSetup(const char* name)
{
    os::SetCurrentThreadName(name);
#if LO_PLATFORM_SWITCH
    // Free to run on any application core (core 2 preferred); new threads
    // otherwise stay on the creating core.
    os::switch_platform::SetThreadIdealCore(0, 2);
#endif
}

uint64_t MovieTimeNs(Movie& movie, uint64_t now)
{
    uint64_t paused = movie.pausedNs;
    if (movie.paused) paused += now - movie.pauseStartNs;
    const uint64_t elapsed = now - movie.startNs;
    return elapsed > paused ? elapsed - paused : 0;
}

void LogEnd(Movie& movie, const char* why)
{
    if (movie.loggedEnd || !movie.stream) return;
    movie.loggedEnd = true;
    const uint32_t decoded = movie.stream->DecodedFrames();
    const double seconds = movie.stream->DecodeSeconds();
    LOG_NOTICE("native movie: {} {}: {} frames decoded ({:.1f} fps decode speed), {} shown, {} late{}{}", movie.name, why,
               decoded, seconds > 0 ? decoded / seconds : 0.0, movie.shown, movie.late,
               movie.stream->LastError().empty() ? "" : ", error: ", movie.stream->LastError());
}
}

// Movie start: open the video on the host and have the guest player skip it.
PPC_FUNC(sub_82815060)
{
    const uint32_t m = ctx.r3.u32;
    std::unique_ptr<Movie> previous;
    {
        std::lock_guard lock(g_mutex);
        previous = std::move(g_movie);
    }
    if (previous)
    {
        LogEnd(*previous, "replaced");
        previous.reset(); // stops its decoder thread
    }
    if (!Enabled())
    {
        __imp__sub_82815060(ctx, base);
        return;
    }

    auto movie = std::make_unique<Movie>();
    const uint32_t info = PPC_LOAD_U32(m + 44);
    const int32_t videoTrack = info ? int32_t(PPC_LOAD_U32(info + 64)) : -1;
    movie->name = info ? MovieName(base, info) : std::string();
    std::string error;
    Location location;
    if (movie->name.empty())
        error = "no movie name";
    else if (videoTrack == -1)
        error = "movie has no video track";
    else if (Locate(movie->name, location, error))
    {
        movie->stream = std::make_unique<movie::WmvStream>();
        if (!movie->stream->Open(location.archive, location.offset, location.size, videoTrack, error))
            movie->stream.reset();
    }
    if (!movie->stream)
    {
        LOG_WARNING("native movie: {} uses the guest decoder ({})", movie->name.empty() ? "?" : movie->name, error);
        __imp__sub_82815060(ctx, base);
        return;
    }

    t_pending = movie.get();
    __imp__sub_82815060(ctx, base);
    t_pending = nullptr;

    const uint32_t player = PPC_LOAD_U32(m + 20);
    if (!movie->player || movie->player != player)
    {
        // The factory fell back to the guest decoder (or failed outright).
        LOG_WARNING("native movie: {} uses the guest decoder (audio-only player unavailable)", movie->name);
        return;
    }
    movie->stream->Start(DecoderThreadSetup);
    LOG_NOTICE("native movie: {} {}x{} video track {} at {}+{:#x}, player {:#x}", movie->name, movie->stream->Width(),
               movie->stream->Height(), videoTrack, location.archive, location.offset, player);
    std::lock_guard lock(g_mutex);
    g_movie = std::move(movie);
}

// Player factory: create it without the video track; on failure, as asked.
PPC_FUNC(sub_82CE9728)
{
    Movie* movie = t_pending;
    if (!movie)
    {
        __imp__sub_82CE9728(ctx, base);
        return;
    }
    const uint32_t r3 = ctx.r3.u32, params = ctx.r4.u32, out = ctx.r5.u32;
    const uint32_t video = PPC_LOAD_U32(params + 28);
    PPC_STORE_U32(params + 28, 0xFFFFFFFFu);
    __imp__sub_82CE9728(ctx, base);
    PPC_STORE_U32(params + 28, video);
    const uint32_t player = out ? PPC_LOAD_U32(out) : 0;
    if (ctx.r3.s32 >= 0 && player)
    {
        movie->player = player;
        return;
    }
    LOG_WARNING("native movie: audio-only player failed ({:#x}); retrying with video", ctx.r3.u32);
    t_pending = nullptr; // sub_82CE9100 must report the guest's own size
    ctx.r3.u64 = r3;
    ctx.r4.u64 = params;
    ctx.r5.u64 = out;
    __imp__sub_82CE9728(ctx, base);
}

// Video size for the textures: the host stream's (the player has no video).
PPC_FUNC(sub_82CE9100)
{
    Movie* movie = t_pending;
    if (!movie || !movie->player || ctx.r3.u32 != movie->player)
    {
        __imp__sub_82CE9100(ctx, base);
        return;
    }
    const uint32_t out = ctx.r4.u32;
    const float fps = 29.97f;
    uint32_t fpsBits;
    std::memcpy(&fpsBits, &fps, 4);
    PPC_STORE_U32(out + 0, movie->stream->Width());
    PPC_STORE_U32(out + 4, movie->stream->Height());
    PPC_STORE_U32(out + 8, fpsBits);
    PPC_STORE_U32(out + 12, 0);
    ctx.r3.u64 = 0;
}

// Player status: an audio-only player must not stop the frame update.
PPC_FUNC(sub_823F02A8)
{
    const uint32_t player = ctx.r3.u32;
    __imp__sub_823F02A8(ctx, base);
    if (ctx.r3.s32 >= 0) return;
    std::lock_guard lock(g_mutex);
    if (g_movie && g_movie->player == player) ctx.r3.u64 = 0;
}

// The stock GetNextFrame (sub_82CF8C38, player vtable +76) also runs the
// player: on the first call (state 1, or 7 after an end) it prepares the
// pipeline and resumes the player's worker threads, which decode and play the
// audio; every call it copies the pipeline status into the player state; at
// the end of the streams it reports 0x16660026. It cannot run here (it would
// wait for video samples of a player without video), so these do the same
// steps through the player's own vtable. Offsets from sub_82CF8C38.
static uint32_t Virtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t offset, uint32_t r4 = 0)
{
    ctx.r3.u64 = object;
    ctx.r4.u64 = r4;
    const uint32_t function = PPC_LOAD_U32(PPC_LOAD_U32(object) + offset);
    PPC_CALL_INDIRECT_FUNC(function);
    return ctx.r3.u32;
}

static void ResumeWorker(PPCContext& ctx, uint8_t* base, uint32_t handle)
{
    ctx.r3.u64 = handle;
    sub_82BE27D0(ctx, base); // NtResumeThread wrapper
}

// Returns a negative HRESULT when the player could not start.
static int32_t StartGuestPlayer(PPCContext& ctx, uint8_t* base, uint32_t player)
{
    const int32_t hr = int32_t(Virtual(ctx, base, player, 160)); // prepare
    if (hr < 0) return hr;
    Virtual(ctx, base, player, 12);      // lock
    Virtual(ctx, base, player, 224);     // set the run event
    Virtual(ctx, base, player, 264, 2);
    Virtual(ctx, base, player, 268, 2);  // state: started
    ResumeWorker(ctx, base, PPC_LOAD_U32(player + 264));
    ResumeWorker(ctx, base, PPC_LOAD_U32(player + 268));
    PPC_STORE_U32(player + 240, 1);
    Virtual(ctx, base, player, 20);      // unlock
    return hr;
}

static void EndGuestPlayer(PPCContext& ctx, uint8_t* base, uint32_t player)
{
    Virtual(ctx, base, player, 268, 7);  // state: ended
    if (PPC_LOAD_U32(player + 240))
    {
        ResumeWorker(ctx, base, PPC_LOAD_U32(player + 272));
        PPC_STORE_U32(player + 240, 0);
    }
}

// Next frame (render thread): run the guest player (audio), then copy the
// host frame due now into the locked Y/U/V textures. Never waits for the decoder.
PPC_FUNC(sub_823F0FA8)
{
    const uint32_t player = ctx.r3.u32, desc = ctx.r4.u32;
    bool host = false, startGuest = false;
    {
        std::lock_guard lock(g_mutex);
        if (g_movie && player && g_movie->player == player && !g_movie->ended)
        {
            host = true;
            startGuest = !g_movie->guestStarted;
        }
    }
    if (!host)
    {
        std::unique_lock lock(g_mutex);
        const bool ended = g_movie && player && g_movie->player == player;
        lock.unlock();
        if (ended)
        {
            ctx.r3.u64 = kEndOfMovie;
            return;
        }
        __imp__sub_823F0FA8(ctx, base);
        return;
    }
    PPC_STORE_U32(desc + 8, 0); // never "late": the caller would loop

    // Guest player steps, without holding g_mutex (they call guest code).
    const uint32_t state = Virtual(ctx, base, player, 248);
    if (startGuest && (state == 1 || state == 7))
    {
        const int32_t hr = StartGuestPlayer(ctx, base, player);
        std::lock_guard lock(g_mutex);
        if (!g_movie || g_movie->player != player) return;
        if (hr < 0)
        {
            if (!g_movie->startFailed)
                LOG_WARNING("native movie: {} guest player start failed ({:#x}); retrying", g_movie->name, uint32_t(hr));
            g_movie->startFailed = true;
            ctx.r3.u64 = uint32_t(hr);
            return;
        }
        g_movie->guestStarted = true;
        LOG_NOTICE("native movie: {} guest player started (state {})", g_movie->name, state);
    }
    const uint32_t status = Virtual(ctx, base, player, 260);
    if (status & 6)
    {
        EndGuestPlayer(ctx, base, player);
        std::lock_guard lock(g_mutex);
        if (g_movie && g_movie->player == player)
        {
            g_movie->ended = true;
            LogEnd(*g_movie, "finished");
        }
        ctx.r3.u64 = kEndOfMovie;
        return;
    }
    Virtual(ctx, base, player, 268, (status & 1) ? 5 : 3);

    std::unique_lock lock(g_mutex);
    Movie* movie = g_movie.get();
    if (!movie || movie->player != player)
    {
        ctx.r3.u64 = kNoNewFrame;
        return;
    }
    const uint64_t now = host_ui::GetActiveGameTimeNs();
    if (!movie->started)
    {
        movie->started = true;
        movie->startNs = now;
    }
    const uint32_t timeMs = uint32_t(std::min<uint64_t>(MovieTimeNs(*movie, now) / 1000000, 0xFFFFFFFFu));
    bool fresh = false;
    const movie::VideoFrame* frame = movie->stream->FrameAt(timeMs, fresh);
    if (!fresh || !frame)
    {
        // The guest player ends the movie when its audio ends; stop waiting
        // for that 10 s after the last video frame.
        if (movie->stream->Finished() && timeMs > movie->lastPts + 10000)
        {
            movie->ended = true;
            LogEnd(*movie, "finished (video)");
            lock.unlock();
            EndGuestPlayer(ctx, base, player);
            ctx.r3.u64 = kEndOfMovie;
        }
        else
            ctx.r3.u64 = kNoNewFrame;
        return;
    }
    const uint32_t w = frame->width, h = frame->height;
    const uint32_t y = PPC_LOAD_U32(desc + 12), u = PPC_LOAD_U32(desc + 16), v = PPC_LOAD_U32(desc + 20);
    const uint32_t yPitch = PPC_LOAD_U32(desc + 36), uPitch = PPC_LOAD_U32(desc + 40), vPitch = PPC_LOAD_U32(desc + 44);
    if (!y || !u || !v || yPitch < w || uPitch < w / 2 || vPitch < w / 2)
    {
        ctx.r3.u64 = kNoNewFrame;
        return;
    }
    for (uint32_t row = 0; row < h; ++row)
        std::memcpy(base + y + size_t(row) * yPitch, frame->y.data() + size_t(row) * w, w);
    for (uint32_t row = 0; row < h / 2; ++row)
    {
        std::memcpy(base + u + size_t(row) * uPitch, frame->u.data() + size_t(row) * (w / 2), w / 2);
        std::memcpy(base + v + size_t(row) * vPitch, frame->v.data() + size_t(row) * (w / 2), w / 2);
    }
    // A frame more than two frame times old: the decoder is behind.
    if (timeMs > frame->ptsMs + 67) ++movie->late;
    if (movie->shown++ == 0)
        LOG_NOTICE("native movie: {} first frame at {} ms", movie->name, timeMs);
    movie->lastPts = frame->ptsMs;
    ctx.r3.u64 = 0;
}

PPC_FUNC(sub_82CE9248)
{
    const uint32_t player = ctx.r3.u32;
    __imp__sub_82CE9248(ctx, base);
    std::lock_guard lock(g_mutex);
    if (g_movie && g_movie->player == player && g_movie->started && !g_movie->paused)
    {
        g_movie->paused = true;
        g_movie->pauseStartNs = host_ui::GetActiveGameTimeNs();
    }
}

PPC_FUNC(sub_82CE9FD8)
{
    const uint32_t player = ctx.r3.u32;
    __imp__sub_82CE9FD8(ctx, base);
    std::lock_guard lock(g_mutex);
    if (g_movie && g_movie->player == player && g_movie->paused)
    {
        g_movie->paused = false;
        g_movie->pausedNs += host_ui::GetActiveGameTimeNs() - g_movie->pauseStartNs;
    }
}

#endif
