#pragma once
#include "asf_demux.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Host decoding of a cutscene's WMV3 video track (FFmpeg's VC-1 decoder) on
// its own thread, a few frames ahead of the movie clock.
namespace movie
{
struct VideoFrame
{
    uint32_t ptsMs = 0;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> y, u, v; // tightly packed planes, I420 (u = Cb, v = Cr)
};

class WmvStream
{
public:
    ~WmvStream();
    // path: host archive; offset/size: the WMV inside it; videoStream: ASF stream
    // number from the game's FMVInfo (-1: first video stream).
    bool Open(const std::string& path, uint64_t offset, uint64_t size, int videoStream, std::string& error);
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    // Thread name for the CPU profiler (set before Start; may be empty).
    void Start(void (*nameThread)(const char*) = nullptr);
    void Stop();

    // The newest decoded frame due at timeMs (pts <= timeMs), discarding older
    // ones; nullptr while none is due. The pointer stays valid until the next
    // call. fresh: true the first time a frame is returned.
    const VideoFrame* FrameAt(uint32_t timeMs, bool& fresh);
    // The decoder reached the end of the stream and every frame was taken.
    bool Finished();
    // Decoder statistics since Start: frames decoded and their total time.
    uint32_t DecodedFrames() const { return decoded_.load(); }
    double DecodeSeconds() const { return decodeNs_.load() / 1e9; }
    std::string LastError();

private:
    void Run();

    static constexpr size_t kQueueFrames = 6;
    std::FILE* file_ = nullptr;
    AsfDemuxer demux_;
    uint32_t width_ = 0, height_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<VideoFrame>> queue_;
    std::vector<std::unique_ptr<VideoFrame>> pool_;
    std::unique_ptr<VideoFrame> current_;
    bool stop_ = false, ended_ = false;
    std::string error_;
    std::atomic<uint32_t> decoded_{0};
    std::atomic<uint64_t> decodeNs_{0};
};
}
