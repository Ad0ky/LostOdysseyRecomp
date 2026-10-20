#include "asf_demux.h"
#include <cstring>
#include <limits>

namespace movie
{
namespace
{
// ASF object GUIDs in file byte order.
constexpr uint8_t kHeaderGuid[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kDataGuid[16] = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kFilePropertiesGuid[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kStreamPropertiesGuid[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kHeaderExtensionGuid[16] = {0xB5, 0x03, 0xBF, 0x5F, 0x2E, 0xA9, 0xCF, 0x11, 0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kExtendedStreamPropertiesGuid[16] = {0xCB, 0xA5, 0xE6, 0x14, 0x72, 0xC6, 0x32, 0x43, 0x83, 0x99, 0xA9, 0x69, 0x52, 0x06, 0x5B, 0x5A};
constexpr uint8_t kVideoMediaGuid[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uint32_t kMaxMediaObjectSize = 64 * 1024 * 1024;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }

// Reads a field whose size comes from a 2-bit length type (0, 1, 2 or 4 bytes).
bool Field(const uint8_t*& p, const uint8_t* end, unsigned type, uint32_t& value)
{
    static constexpr unsigned kSizes[4] = {0, 1, 2, 4};
    const unsigned size = kSizes[type & 3];
    if (size_t(end - p) < size) return false;
    value = size == 0 ? 0 : size == 1 ? p[0] : size == 2 ? Le16(p) : Le32(p);
    p += size;
    return true;
}
}

bool AsfDemuxer::ReadAt(uint64_t position, void* out, size_t size)
{
    if (!file_ || position > size_ || size > size_ - position || position > uint64_t(std::numeric_limits<int64_t>::max()) ||
        base_ > uint64_t(std::numeric_limits<int64_t>::max()) - position) return false;
    const int64_t absolute = int64_t(base_ + position);
#if defined(_WIN32)
    if (_fseeki64(file_, absolute, SEEK_SET) != 0) return false;
#else
    if (fseeko(file_, absolute, SEEK_SET) != 0) return false;
#endif
    return std::fread(out, 1, size, file_) == size;
}

bool AsfDemuxer::Open(std::FILE* file, uint64_t offset, uint64_t size, int wantedStream, std::string& error)
{
    *this = AsfDemuxer{};
    error.clear();
    auto fail = [&](const char* message) {
        error_ = message;
        error = error_;
        return false;
    };
    file_ = file;
    base_ = offset;
    size_ = size;
    uint8_t top[30];
    if (!ReadAt(0, top, sizeof(top)) || std::memcmp(top, kHeaderGuid, 16) != 0)
        return fail("not an ASF file");
    const uint64_t headerSize = Le64(top + 16);
    if (headerSize < 30 || headerSize > 16 * 1024 * 1024 || headerSize > size_)
        return fail("bad ASF header size");
    std::vector<uint8_t> header(static_cast<size_t>(headerSize));
    if (!ReadAt(0, header.data(), header.size()))
        return fail("short ASF header");
    if (!ParseHeader(header, wantedStream))
    {
        error = error_.empty() ? "bad ASF header" : error_;
        error_ = error;
        return false;
    }
    // Data object directly after the header.
    uint8_t data[50];
    if (!ReadAt(headerSize, data, sizeof(data)) || std::memcmp(data, kDataGuid, 16) != 0)
        return fail("ASF data object missing");
    const uint64_t dataSize = Le64(data + 16);
    if (dataSize < 50 || dataSize > size_ - headerSize)
        return fail("bad ASF data object size");
    dataStart_ = headerSize + 50;
    dataEnd_ = headerSize + dataSize;
    nextPacket_ = dataStart_;
    if (!packetSize_ || !video_.streamNumber)
        return fail(!packetSize_ ? "ASF packets are not fixed-size" : "no video stream");
    if ((dataEnd_ - dataStart_) % packetSize_ != 0)
        return fail("incomplete ASF data packet");
    packet_.resize(packetSize_);
    return true;
}

bool AsfDemuxer::ParseStreamProperties(const uint8_t* object, size_t size, int wantedStream)
{
    if (size < 78) return false;
    if (std::memcmp(object + 24, kVideoMediaGuid, 16) != 0) return true; // not video
    const uint32_t typeSpecificSize = Le32(object + 64);
    const uint32_t number = Le16(object + 72) & 0x7F;
    if (!number || 78 + uint64_t(typeSpecificSize) > size || typeSpecificSize < 11 + 40) return false;
    if (wantedStream > 0 && int(number) != wantedStream) return true;
    if (video_.streamNumber) return true;
    const uint8_t* t = object + 78;
    const uint16_t formatSize = Le16(t + 9);
    const uint8_t* bih = t + 11;
    if (formatSize < 40 || 11 + uint32_t(formatSize) > typeSpecificSize) return false;
    const uint32_t biSize = Le32(bih);
    if (biSize < 40 || biSize > formatSize) return false;
    video_.streamNumber = number;
    video_.width = Le32(bih + 4);
    video_.height = Le32(bih + 8);
    video_.fourcc = Le32(bih + 16);
    video_.extradata.clear();
    if (biSize > 40)
        video_.extradata.assign(bih + 40, bih + biSize);
    return true;
}

bool AsfDemuxer::ParseHeader(const std::vector<uint8_t>& header, int wantedStream)
{
    const uint32_t objectCount = Le32(header.data() + 24);
    size_t pos = 30;
    for (uint32_t index = 0; index < objectCount; ++index)
    {
        if (header.size() - pos < 24) return false;
        const uint8_t* object = header.data() + pos;
        const uint64_t objectSize = Le64(object + 16);
        if (objectSize < 24 || objectSize > header.size() - pos) return false;
        if (std::memcmp(object, kFilePropertiesGuid, 16) == 0 && objectSize >= 104)
        {
            prerollMs_ = Le64(object + 80);
            const uint32_t minPacket = Le32(object + 92), maxPacket = Le32(object + 96);
            if (minPacket == maxPacket && minPacket >= 32 && minPacket <= 1024 * 1024) packetSize_ = minPacket;
        }
        else if (std::memcmp(object, kStreamPropertiesGuid, 16) == 0)
        {
            if (!ParseStreamProperties(object, size_t(objectSize), wantedStream)) return false;
        }
        else if (std::memcmp(object, kHeaderExtensionGuid, 16) == 0)
        {
            if (objectSize < 46) return false;
            // Extended Stream Properties objects may embed a Stream Properties object.
            size_t sub = 46;
            while (sub < objectSize)
            {
                if (objectSize - sub < 24) return false;
                const uint8_t* e = object + sub;
                const uint64_t eSize = Le64(e + 16);
                if (eSize < 24 || eSize > objectSize - sub) return false;
                if (std::memcmp(e, kExtendedStreamPropertiesGuid, 16) == 0)
                {
                    if (eSize < 88) return false;
                    size_t p = 88;
                    const uint16_t names = Le16(e + 84), systems = Le16(e + 86);
                    bool ok = true;
                    for (uint16_t i = 0; ok && i < names; ++i)
                    {
                        if (eSize - p < 4) { ok = false; break; }
                        const size_t nameSize = Le16(e + p + 2);
                        if (nameSize > eSize - p - 4) { ok = false; break; }
                        p += 4 + nameSize;
                    }
                    for (uint16_t i = 0; ok && i < systems; ++i)
                    {
                        if (eSize - p < 22) { ok = false; break; }
                        const uint32_t systemSize = Le32(e + p + 18);
                        if (systemSize > eSize - p - 22) { ok = false; break; }
                        p += 22 + systemSize;
                    }
                    if (!ok) return false;
                    if (eSize - p >= 24 && std::memcmp(e + p, kStreamPropertiesGuid, 16) == 0)
                    {
                        const uint64_t spSize = Le64(e + p + 16);
                        if (spSize < 24 || spSize > eSize - p ||
                            !ParseStreamProperties(e + p, size_t(spSize), wantedStream)) return false;
                    }
                }
                sub += size_t(eSize);
            }
        }
        pos += size_t(objectSize);
    }
    return pos == header.size();
}

bool AsfDemuxer::Deliver(uint32_t objectNumber, uint32_t objectSize, uint32_t offset, uint32_t presentationMs,
                         bool key, const uint8_t* data, size_t size)
{
    if (offset == 0)
    {
        if (assembling_) { error_ = "incomplete ASF media object"; return false; }
        if (!objectSize || objectSize > kMaxMediaObjectSize)
        {
            error_ = "unsupported ASF media object size";
            return false;
        }
        assembling_ = true;
        objectNumber_ = objectNumber;
        objectSize_ = objectSize;
        objectPts_ = presentationMs;
        objectKey_ = key;
        object_.clear();
    }
    if (!assembling_ || objectNumber != objectNumber_ || offset != object_.size() ||
        offset > objectSize_ ||
        size > objectSize_ - offset)
    {
        error_ = "invalid ASF media object fragment";
        return false;
    }
    object_.insert(object_.end(), data, data + size);
    if (object_.size() == objectSize_)
    {
        AsfFrame frame;
        frame.data = std::move(object_);
        frame.ptsMs = uint32_t(objectPts_ >= prerollMs_ ? objectPts_ - prerollMs_ : 0);
        frame.key = objectKey_;
        ready_.push_back(std::move(frame));
        object_ = {};
        assembling_ = false;
    }
    return true;
}

bool AsfDemuxer::ReadPacket()
{
    auto fail = [&](const char* message) { error_ = message; return false; };
    if (nextPacket_ == dataEnd_)
    {
        if (assembling_) return fail("incomplete ASF media object");
        return false;
    }
    if (dataEnd_ - nextPacket_ < packetSize_ || !ReadAt(nextPacket_, packet_.data(), packetSize_))
        return fail("cannot read ASF data packet");
    nextPacket_ += packetSize_;
    const uint8_t* p = packet_.data();
    const uint8_t* end = p + packetSize_;
    if (*p & 0x80) // error correction data
    {
        const unsigned length = *p & 0x0F;
        if (size_t(end - p) < 1 + length) return fail("invalid ASF error correction data");
        p += 1 + length;
    }
    if (end - p < 2) return fail("short ASF packet header");
    const uint8_t lengthFlags = p[0], propertyFlags = p[1];
    p += 2;
    uint32_t packetLength = 0, sequence = 0, padding = 0;
    if (!Field(p, end, lengthFlags >> 5, packetLength) || !Field(p, end, lengthFlags >> 1, sequence) ||
        !Field(p, end, lengthFlags >> 3, padding) || end - p < 6)
        return fail("short ASF packet header");
    p += 6; // send time, duration
    if (packetLength == 0) packetLength = packetSize_;
    if (packetLength > packetSize_ || packetLength < size_t(p - packet_.data()))
        return fail("invalid ASF packet length");
    const uint8_t* payloadEnd = packet_.data() + packetLength;
    if (padding > size_t(payloadEnd - p)) return fail("invalid ASF packet padding");
    payloadEnd -= padding;
    const bool multiple = lengthFlags & 1;
    unsigned count = 1, payloadLengthType = 0;
    if (multiple)
    {
        if (p >= payloadEnd) return fail("short ASF payload header");
        count = *p & 0x3F;
        payloadLengthType = *p >> 6;
        ++p;
        if (!count || !payloadLengthType) return fail("unsupported ASF payload length type");
    }
    for (unsigned i = 0; i < count; ++i)
    {
        if (p >= payloadEnd) return fail("short ASF payload");
        const uint8_t streamByte = *p++;
        const uint32_t stream = streamByte & 0x7F;
        const bool key = streamByte & 0x80;
        uint32_t objectNumber = 0, offset = 0, replicatedLength = 0;
        if (!Field(p, payloadEnd, propertyFlags >> 4, objectNumber) || !Field(p, payloadEnd, propertyFlags >> 2, offset) ||
            !Field(p, payloadEnd, propertyFlags, replicatedLength) || replicatedLength > size_t(payloadEnd - p))
            return fail("invalid ASF payload header");
        const uint8_t* replicated = p;
        p += replicatedLength;
        uint32_t length = 0;
        if (multiple)
        {
            if (!Field(p, payloadEnd, payloadLengthType, length)) return fail("short ASF payload length");
        }
        else
            length = uint32_t(payloadEnd - p);
        if (!length || length > size_t(payloadEnd - p)) return fail("invalid ASF payload length");
        const uint8_t* data = p;
        p += length;
        if (stream != video_.streamNumber) continue;
        if (replicatedLength == 1)
        {
            // Compressed payload: offset is the presentation time, then
            // sub-payloads of {length byte, whole media object}.
            const uint32_t delta = replicated[0];
            uint32_t time = offset;
            const uint8_t* s = data;
            const uint8_t* sEnd = data + length;
            while (s < sEnd)
            {
                const uint32_t subLength = *s++;
                if (subLength > size_t(sEnd - s)) return fail("invalid ASF compressed payload");
                if (!Deliver(objectNumber, subLength, 0, time, key, s, subLength)) return false;
                s += subLength;
                ++objectNumber;
                time += delta;
            }
        }
        else if (replicatedLength >= 8)
        {
            const uint32_t objectSize = Le32(replicated);
            if (offset != 0 && (!assembling_ || objectSize != objectSize_ ||
                                Le32(replicated + 4) != objectPts_))
                return fail("invalid ASF media object fragment");
            if (!Deliver(objectNumber, objectSize, offset, Le32(replicated + 4), key, data, length)) return false;
        }
        else if (replicatedLength == 0)
        {
            if (!assembling_ || objectNumber != objectNumber_ || offset == 0)
                return fail("invalid ASF media object continuation");
            if (!Deliver(objectNumber, objectSize_, offset, objectPts_, objectKey_, data, length)) return false;
        }
        else return fail("unsupported ASF replicated data");
    }
    if (p != payloadEnd) return fail("trailing ASF payload data");
    return true;
}

bool AsfDemuxer::Next(AsfFrame& frame)
{
    if (!file_ || !error_.empty()) return false;
    while (readyPos_ >= ready_.size())
    {
        ready_.clear();
        readyPos_ = 0;
        if (!ReadPacket()) return false;
    }
    frame = std::move(ready_[readyPos_++]);
    return true;
}
}
