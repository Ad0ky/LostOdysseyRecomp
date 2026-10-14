// Built only with the WMV3 decoder (Switch; see thirdparty/ffmpeg.cmake).
#if LO_HAS_WMV3_DECODER
#include "wmv_stream.h"
#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
// Referenced directly: the FFmpeg build registers only the XMA decoder by name.
extern AVCodec ff_wmv3_decoder;
}

namespace movie
{
WmvStream::~WmvStream()
{
    Stop();
    if (file_) std::fclose(file_);
}

bool WmvStream::Open(const std::string& path, uint64_t offset, uint64_t size, int videoStream, std::string& error)
{
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_)
    {
        error = "cannot open " + path;
        return false;
    }
    // Large reads: the demuxer reads one ASF packet (a few KiB) at a time.
    std::setvbuf(file_, nullptr, _IOFBF, 256 * 1024);
    if (!demux_.Open(file_, offset, size, videoStream, error)) return false;
    const auto& video = demux_.Video();
    if (video.fourcc != 0x33564D57) // 'WMV3'
    {
        error = "video is not WMV3";
        return false;
    }
    if (!video.width || !video.height || video.width > 4096 || video.height > 4096 || (video.width | video.height) & 1)
    {
        error = "unsupported video size";
        return false;
    }
    width_ = video.width;
    height_ = video.height;
    return true;
}

void WmvStream::Start(void (*nameThread)(const char*))
{
    thread_ = std::thread([this, nameThread] {
        if (nameThread) nameThread("Movie Decoder");
        Run();
    });
}

void WmvStream::Stop()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::string WmvStream::LastError()
{
    std::lock_guard lock(mutex_);
    return error_;
}

void WmvStream::Run()
{
    const auto& video = demux_.Video();
    AVCodecContext* context = avcodec_alloc_context3(&ff_wmv3_decoder);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto fail = [&](const char* message) {
        std::lock_guard lock(mutex_);
        error_ = message;
        ended_ = true;
    };
    if (!context || !packet || !frame)
    {
        fail("out of memory");
    }
    else
    {
        context->width = int(video.width);
        context->height = int(video.height);
        context->codec_tag = video.fourcc;
        context->thread_count = 1;
        if (!video.extradata.empty())
        {
            context->extradata = static_cast<uint8_t*>(av_mallocz(video.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (context->extradata)
            {
                std::memcpy(context->extradata, video.extradata.data(), video.extradata.size());
                context->extradata_size = int(video.extradata.size());
            }
        }
        if (avcodec_open2(context, &ff_wmv3_decoder, nullptr) < 0)
            fail("cannot open the WMV3 decoder");
    }

    auto deliver = [&]() -> bool {
        // Wait for room in the queue, then copy the decoded picture out.
        std::unique_ptr<VideoFrame> out;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || queue_.size() < kQueueFrames; });
            if (stop_) return false;
            if (!pool_.empty())
            {
                out = std::move(pool_.back());
                pool_.pop_back();
            }
        }
        if (!out) out = std::make_unique<VideoFrame>();
        const uint32_t w = uint32_t(frame->width), h = uint32_t(frame->height);
        if (frame->format != AV_PIX_FMT_YUV420P || w != width_ || h != height_) return true; // skip odd frames
        out->width = w;
        out->height = h;
        const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        out->ptsMs = pts > 0 ? uint32_t(pts) : 0;
        out->y.resize(size_t(w) * h);
        out->u.resize(size_t(w / 2) * (h / 2));
        out->v.resize(size_t(w / 2) * (h / 2));
        for (uint32_t row = 0; row < h; ++row)
            std::memcpy(out->y.data() + size_t(row) * w, frame->data[0] + size_t(row) * frame->linesize[0], w);
        for (uint32_t row = 0; row < h / 2; ++row)
        {
            std::memcpy(out->u.data() + size_t(row) * (w / 2), frame->data[1] + size_t(row) * frame->linesize[1], w / 2);
            std::memcpy(out->v.data() + size_t(row) * (w / 2), frame->data[2] + size_t(row) * frame->linesize[2], w / 2);
        }
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(out));
        }
        return true;
    };

    bool ok = context && packet && frame && avcodec_is_open(context);
    AsfFrame input;
    while (ok)
    {
        {
            std::lock_guard lock(mutex_);
            if (stop_) break;
        }
        const bool more = demux_.Next(input);
        const auto start = std::chrono::steady_clock::now();
        int sent;
        if (more)
        {
            if (av_new_packet(packet, int(input.data.size())) < 0) { fail("out of memory"); break; }
            std::memcpy(packet->data, input.data.data(), input.data.size());
            packet->pts = packet->dts = input.ptsMs;
            if (input.key) packet->flags |= AV_PKT_FLAG_KEY;
            sent = avcodec_send_packet(context, packet);
            av_packet_unref(packet);
        }
        else
            sent = avcodec_send_packet(context, nullptr); // drain
        (void)sent; // a corrupt frame is skipped, like the console player does
        for (;;)
        {
            const int received = avcodec_receive_frame(context, frame);
            if (received < 0) break;
            decodeNs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            ++decoded_;
            const bool kept = deliver();
            av_frame_unref(frame);
            if (!kept) { ok = false; break; }
        }
        if (!more) break;
    }
    {
        std::lock_guard lock(mutex_);
        ended_ = true;
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
}

const VideoFrame* WmvStream::FrameAt(uint32_t timeMs, bool& fresh)
{
    fresh = false;
    std::unique_ptr<VideoFrame> newest;
    {
        std::lock_guard lock(mutex_);
        while (!queue_.empty() && queue_.front()->ptsMs <= timeMs)
        {
            if (newest) pool_.push_back(std::move(newest));
            newest = std::move(queue_.front());
            queue_.pop_front();
        }
        if (newest)
        {
            if (current_) pool_.push_back(std::move(current_));
            current_ = std::move(newest);
            fresh = true;
        }
    }
    if (fresh) cv_.notify_all();
    return current_.get();
}

bool WmvStream::Finished()
{
    std::lock_guard lock(mutex_);
    return ended_ && queue_.empty();
}
}
#endif
