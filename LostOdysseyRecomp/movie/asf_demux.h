#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Minimal ASF (WMV) demuxer for one video stream: enough for the game's
// cutscene files (WMV3 Main + WMA Pro, fixed-size data packets). Reads the
// file sequentially from a host FILE at a base offset (a file inside an FPD
// archive), reassembles the video stream's media objects and returns them in
// file order. Audio payloads are skipped; the guest still plays the audio.
namespace movie
{
struct AsfVideoInfo
{
    uint32_t streamNumber = 0;
    uint32_t width = 0, height = 0;
    uint32_t fourcc = 0;
    std::vector<uint8_t> extradata; // BITMAPINFOHEADER extra bytes (WMV3 sequence header)
};

struct AsfFrame
{
    std::vector<uint8_t> data;
    uint32_t ptsMs = 0;   // presentation time in ms, preroll removed
    bool key = false;
};

class AsfDemuxer
{
public:
    // file stays owned by the caller. size: bytes of the ASF file at offset.
    bool Open(std::FILE* file, uint64_t offset, uint64_t size, int wantedStream, std::string& error);
    const AsfVideoInfo& Video() const { return video_; }
    uint64_t PrerollMs() const { return prerollMs_; }
    // Next complete video frame; false at the end of the data (or on error).
    bool Next(AsfFrame& frame);
    std::string Error() const { return error_; }

private:
    bool ReadAt(uint64_t position, void* out, size_t size);
    bool ParseHeader(const std::vector<uint8_t>& header, int wantedStream);
    bool ParseStreamProperties(const uint8_t* object, size_t size, int wantedStream);
    bool ReadPacket();
    bool Deliver(uint32_t objectNumber, uint32_t objectSize, uint32_t offset, uint32_t presentationMs,
                 bool key, const uint8_t* data, size_t size);

    std::FILE* file_ = nullptr;
    uint64_t base_ = 0, size_ = 0;
    uint64_t prerollMs_ = 0;
    uint32_t packetSize_ = 0;
    uint64_t dataStart_ = 0, dataEnd_ = 0, nextPacket_ = 0;
    AsfVideoInfo video_;
    std::vector<uint8_t> packet_;
    // Media object being reassembled.
    bool assembling_ = false;
    uint32_t objectNumber_ = 0, objectSize_ = 0, objectPts_ = 0;
    bool objectKey_ = false;
    std::vector<uint8_t> object_;
    std::vector<AsfFrame> ready_;
    size_t readyPos_ = 0;
    std::string error_;
};
}
