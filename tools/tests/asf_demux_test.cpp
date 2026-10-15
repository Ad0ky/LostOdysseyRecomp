#include "movie/asf_demux.h"

#include <cstdio>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

namespace
{
constexpr uint8_t kHeaderGuid[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kDataGuid[16] = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kFilePropertiesGuid[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kStreamPropertiesGuid[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kVideoMediaGuid[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr size_t kPacketSize = 64;

void Require(bool value, const char* why)
{
    if (!value) throw std::runtime_error(why);
}

void Put16(Bytes& b, size_t at, uint16_t value)
{
    b.at(at) = uint8_t(value);
    b.at(at + 1) = uint8_t(value >> 8);
}

void Put32(Bytes& b, size_t at, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) b.at(at + i) = uint8_t(value >> (8 * i));
}

void Put64(Bytes& b, size_t at, uint64_t value)
{
    Put32(b, at, uint32_t(value));
    Put32(b, at + 4, uint32_t(value >> 32));
}

void PutGuid(Bytes& b, size_t at, const uint8_t (&guid)[16])
{
    for (size_t i = 0; i < 16; ++i) b.at(at + i) = guid[i];
}

Bytes FileProperties()
{
    Bytes b(104);
    PutGuid(b, 0, kFilePropertiesGuid);
    Put64(b, 16, b.size());
    Put64(b, 80, 100); // preroll ms
    Put32(b, 92, kPacketSize);
    Put32(b, 96, kPacketSize);
    return b;
}

Bytes StreamProperties(uint16_t stream)
{
    Bytes b(78 + 51);
    PutGuid(b, 0, kStreamPropertiesGuid);
    Put64(b, 16, b.size());
    PutGuid(b, 24, kVideoMediaGuid);
    Put32(b, 64, 51);
    Put16(b, 72, stream);
    Put16(b, 78 + 9, 40); // BITMAPINFOHEADER follows 11-byte video prefix
    Put32(b, 78 + 11, 40);
    Put32(b, 78 + 15, 1280);
    Put32(b, 78 + 19, 720);
    Put32(b, 78 + 27, 0x33564D57); // WMV3
    return b;
}

Bytes Packet(uint8_t stream, uint8_t number, uint8_t offset, uint32_t objectSize,
             uint8_t replicatedLength, const Bytes& payload, bool key = false)
{
    const size_t payloadStart = 15 + replicatedLength;
    Require(payloadStart + payload.size() <= kPacketSize, "fixture payload size");
    Bytes b(kPacketSize);
    b[0] = 0x48; // 16-bit packet length, 8-bit padding length, one payload
    b[1] = 0x15; // 8-bit object number, offset and replicated-data length
    Put16(b, 2, kPacketSize);
    b[4] = uint8_t(kPacketSize - payloadStart - payload.size());
    b[11] = uint8_t(stream | (key ? 0x80 : 0));
    b[12] = number;
    b[13] = offset;
    b[14] = replicatedLength;
    if (replicatedLength >= 8)
    {
        Put32(b, 15, objectSize);
        Put32(b, 19, 105); // presentation time before preroll
    }
    for (size_t i = 0; i < payload.size(); ++i) b[payloadStart + i] = payload[i];
    return b;
}

Bytes Asf(const std::vector<Bytes>& packets)
{
    const Bytes props = FileProperties(), stream1 = StreamProperties(1), stream2 = StreamProperties(2);
    Bytes b(30);
    PutGuid(b, 0, kHeaderGuid);
    Put64(b, 16, b.size() + props.size() + stream1.size() + stream2.size());
    Put32(b, 24, 3);
    b.insert(b.end(), props.begin(), props.end());
    b.insert(b.end(), stream1.begin(), stream1.end());
    b.insert(b.end(), stream2.begin(), stream2.end());
    const size_t dataAt = b.size();
    b.resize(b.size() + 50);
    PutGuid(b, dataAt, kDataGuid);
    Put64(b, dataAt + 16, 50 + packets.size() * kPacketSize);
    for (const Bytes& packet : packets) b.insert(b.end(), packet.begin(), packet.end());
    return b;
}

struct File
{
    std::unique_ptr<std::FILE, decltype(&std::fclose)> handle{std::tmpfile(), &std::fclose};

    void Write(const Bytes& bytes, uint64_t offset = 0)
    {
        Require(bool(handle), "create fixture file");
#if defined(_WIN32)
        Require(_fseeki64(handle.get(), int64_t(offset), SEEK_SET) == 0, "seek fixture file");
#else
        Require(fseeko(handle.get(), int64_t(offset), SEEK_SET) == 0, "seek fixture file");
#endif
        Require(std::fwrite(bytes.data(), 1, bytes.size(), handle.get()) == bytes.size(), "write fixture file");
        Require(std::fflush(handle.get()) == 0, "flush fixture file");
    }
};

void ReassemblyAndSelection()
{
    const Bytes input = Asf({Packet(1, 1, 0, 1, 8, {'A'}),
                             Packet(2, 7, 0, 6, 8, {'a', 'b', 'c'}, true),
                             Packet(1, 2, 0, 1, 8, {'B'}),
                             Packet(2, 7, 3, 0, 0, {'d', 'e', 'f'})});
    File file;
    file.Write(input);
    movie::AsfDemuxer demux;
    movie::AsfFrame frame;
    std::string error;
    Require(demux.Open(file.handle.get(), 0, input.size(), 2, error), "open selected stream");
    Require(demux.Video().streamNumber == 2 && demux.PrerollMs() == 100, "stream metadata");
    Require(demux.Next(frame) && frame.data == Bytes({'a', 'b', 'c', 'd', 'e', 'f'}) &&
            frame.ptsMs == 5 && frame.key, "reassembled zero-replication continuation");
    Require(!demux.Next(frame) && demux.Error().empty(), "clean end of selected stream");

    Require(demux.Open(file.handle.get(), 0, input.size(), -1, error), "reuse for first stream");
    Require(demux.Video().streamNumber == 1, "first stream selected");
    Require(demux.Next(frame) && frame.data == Bytes({'A'}), "first stream first frame");
    Require(demux.Next(frame) && frame.data == Bytes({'B'}), "first stream second frame");
    Require(!demux.Next(frame) && demux.Error().empty(), "reuse cleared queued frames");

    Require(!demux.Open(file.handle.get(), 0, input.size(), 3, error) &&
            error == "no video stream" && demux.Error() == error, "missing selected stream");
    Require(demux.Open(file.handle.get(), 0, input.size(), 2, error) && error.empty(), "reuse after failure");
    Require(demux.Next(frame) && frame.data.size() == 6, "reuse after failure frame");

    const Bytes repeatedMetadata = Asf({Packet(2, 9, 0, 4, 8, {'w', 'x'}),
                                        Packet(2, 9, 2, 4, 8, {'y', 'z'})});
    File second;
    second.Write(repeatedMetadata);
    Require(demux.Open(second.handle.get(), 0, repeatedMetadata.size(), 2, error), "open full replicated continuation");
    Require(demux.Next(frame) && frame.data == Bytes({'w', 'x', 'y', 'z'}),
            "reassembled repeated-metadata continuation");
}

void RejectCorruption()
{
    auto check = [](const Bytes& input, const char* why) {
        File file;
        file.Write(input);
        movie::AsfDemuxer demux;
        movie::AsfFrame frame;
        std::string error;
        if (demux.Open(file.handle.get(), 0, input.size(), 2, error))
            Require(!demux.Next(frame) && !demux.Error().empty(), why);
        else
            Require(!error.empty() && demux.Error() == error, why);
    };
    auto packet = Packet(2, 1, 0, 3, 8, {'a', 'b', 'c'});
    Put16(packet, 2, 10); // packet end precedes the parsed header
    check(Asf({packet}), "packet length before header");
    check(Asf({Packet(2, 1, 0, 64 * 1024 * 1024 + 1, 8, {'a'})}), "oversized object");
    check(Asf({Packet(2, 1, 0, 4, 8, {'a', 'b'}),
               Packet(2, 1, 3, 0, 0, {'c', 'd'})}), "fragment gap");
    check(Asf({Packet(2, 1, 0, 2, 8, {'a', 'b', 'c'})}), "fragment overrun");
    check(Asf({Packet(2, 1, 0, 4, 8, {'a', 'b'})}), "unfinished media object at end");
    check(Asf({Packet(2, 1, 0, 4, 8, {'a', 'b'}),
               Packet(2, 1, 2, 5, 8, {'c', 'd'})}), "changed media object size");
    check(Asf({Packet(2, 1, 1, 0, 0, {'a'})}), "orphan continuation");
    packet = Packet(2, 1, 0, 3, 8, {'a', 'b', 'c'});
    packet[4] = 0xff;
    check(Asf({packet}), "padding beyond packet");
    auto malformedHeader = Asf({Packet(2, 1, 0, 1, 8, {'a'})});
    Put32(malformedHeader, 24, 4);
    check(malformedHeader, "incorrect header object count");
}

void LargeArchiveOffset()
{
    const Bytes input = Asf({Packet(2, 1, 0, 1, 8, {'x'})});
    constexpr uint64_t offset = (uint64_t(2) << 30) + 4096;
    File file;
    file.Write(input, offset);
    movie::AsfDemuxer demux;
    movie::AsfFrame frame;
    std::string error;
    Require(demux.Open(file.handle.get(), offset, input.size(), 2, error), "open above 2 GiB offset");
    Require(demux.Next(frame) && frame.data == Bytes({'x'}), "read above 2 GiB offset");
}
}

int main()
{
    try
    {
        ReassemblyAndSelection();
        RejectCorruption();
        LargeArchiveOffset();
        std::cout << "PASS: ASF demux synthetic fixtures\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
