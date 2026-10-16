#pragma once
#include "fpi_index.h"

#include <algorithm>
#include <map>

// Overflow archives for rebuilt mod files. The game turns a record's sector into
// a 32-bit offset and adds file positions in 32 bits, so nothing past 4 GiB of
// an archive file can be read. Rebuilt files that do not fit after their own
// archive go into extra archives added to the index. Those have no folder, like
// LO.fpd, so their full-path copies of the records are found first: the game
// tries the archives that hold a path by folder length, shortest first
// (sub_82853DA8, sub_828548A0). The original records stay as they are, and
// directory listings merge all archives as they do for DLC (sub_82855050).
namespace modding::fpi
{
// A file of an overflow archive: a copy of the record at `record` (from archive
// `source`) pointing at `offset` of overflow archive `archive`.
struct OverflowFile
{
    size_t record = 0;
    uint32_t source = 0;
    std::string path;     // lower case, '/' separators
    uint32_t archive = 0; // position in the names passed to AddOverflowArchives
    uint64_t offset = 0;  // 2048-byte aligned
    uint32_t size = 0, decoded = 0;
    bool packed = false; // CPX: decoded is the record's +20
};

namespace detail
{
inline void Put16(std::vector<uint8_t> &b, size_t p, uint16_t v)
{
    b[p] = uint8_t(v);
    b[p + 1] = uint8_t(v >> 8);
}
inline void Put32(std::vector<uint8_t> &b, size_t p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b[p + i] = uint8_t(v >> (8 * i));
}
inline size_t Align(size_t v, size_t a) { return (v + a - 1) / a * a; }

// A name in the dictionary's base-40 words (see Names::Unpack).
inline std::vector<uint16_t> Encode(std::string_view name)
{
    std::vector<uint16_t> codes;
    for (const char c : name)
    {
        int code = -1;
        if (c >= '0' && c <= '9') code = 1 + (c - '0');
        else if (c >= 'a' && c <= 'z') code = 11 + (c - 'a');
        else if (c >= 'A' && c <= 'Z') code = 11 + (c - 'A');
        else if (c == '_') code = 37;
        else if (c == '.') code = 38;
        else if (c == '\\') code = 39;
        Require(code >= 0, "name not storable in the FPI dictionary");
        codes.push_back(uint16_t(code));
    }
    Require(!codes.empty() && codes.size() <= 2 + 3 * 39, "FPI name length");
    const size_t count = codes.size() > 2 ? (codes.size() - 2 + 2) / 3 : 0;
    codes.resize(2 + 3 * count, 0);
    std::vector<uint16_t> words{uint16_t(count + 40 * codes[0] + 1600 * codes[1])};
    for (size_t i = 0; i < count; ++i)
        words.push_back(uint16_t(codes[2 + 3 * i] + 40 * codes[3 + 3 * i] + 1600 * codes[4 + 3 * i]));
    return words;
}
}

// index: an index (IndexSize() bytes from the start of the file, as on disc).
// names: lower-case file names of the overflow archives, in the folder of the
// index. Returns the index with the archives added, in whole sectors: the
// dictionary (with any new names) and the archive table move to its end, the
// new records follow. An archive record's offsets are relative to itself, so
// the moved table points back to the old records; the game adds them in 32
// bits (sub_82854400), and Read() does the same. The index stays within the
// game's 512 sectors. Throws std::runtime_error.
inline std::vector<uint8_t> AddOverflowArchives(std::span<const uint8_t> index, const std::vector<std::string> &names,
                                                const std::vector<OverflowFile> &files)
{
    using detail::Le16, detail::Le32, detail::Put16, detail::Put32, detail::Require, detail::Align;
    const auto parsed = Read(index);
    const uint32_t dictionary = Le32(index, 40), extensions = Le32(index, 44), count = Le16(index, 26), table = Le32(index, 32);
    const size_t dictionaryBytes = size_t(Le16(index, dictionary + 2)) * 16;
    Require(dictionaryBytes >= 4 && dictionary + dictionaryBytes <= index.size(), "invalid FPI dictionary");
    Require(count + names.size() <= 64, "too many FPI archives");
    const detail::Names reader{index, dictionary, extensions};

    // Name bits by name: every record and extension of the index, then new words.
    std::map<std::string, uint32_t> known;
    for (uint32_t i = 0; i < index[23]; ++i) // the extensions, by word
        if (const uint32_t word = Le16(index, extensions + 2 * i)) known.emplace(reader.Name(word), word);
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t ar = table + i * 48;
        const size_t base = uint32_t(ar + Le32(index, ar + 4));
        std::vector<std::pair<uint32_t, uint32_t>> pending{{0, Le16(index, ar + 2)}};
        size_t visits = 0;
        while (!pending.empty())
        {
            const auto [first, n] = pending.back();
            pending.pop_back();
            for (uint32_t j = 0; j < n; ++j)
            {
                Require(++visits <= 65536 * 4, "FPI folder cycle");
                const size_t p = base + size_t(first + j) * 24;
                const uint32_t bits = Le32(index, p);
                if (auto name = reader.Name(bits & 0x0fffffff); !name.empty()) known.emplace(std::move(name), bits & 0x0fffffff);
                if (bits & 0x10000000) pending.push_back({Le32(index, p + 20), Le16(index, p + 14)});
            }
        }
    }
    std::vector<uint16_t> words; // appended to the dictionary
    auto bits = [&](const std::string &name) {
        if (const auto it = known.find(name); it != known.end()) return it->second;
        const auto encoded = detail::Encode(name);
        const size_t word = (dictionaryBytes / 2) + words.size();
        Require(word < 0x40000, "FPI dictionary full");
        words.insert(words.end(), encoded.begin(), encoded.end());
        known.emplace(name, uint32_t(word));
        return uint32_t(word);
    };

    // One tree per overflow archive: folders of the paths, files as copies of their records.
    struct Node { std::map<std::string, Node> children; const OverflowFile *file = nullptr; };
    std::vector<Node> roots(names.size());
    std::vector<uint32_t> sources(names.size(), UINT32_MAX);
    for (const auto &file : files)
    {
        Require(file.archive < names.size() && file.record + 24 <= index.size() && file.source < count, "invalid overflow file");
        Require(file.offset % kSector == 0 && file.offset / kSector < (1u << 21) && file.offset + file.size <= (uint64_t(1) << 32),
                "overflow file past 4 GiB");
        if (sources[file.archive] == UINT32_MAX) sources[file.archive] = file.source;
        Node *node = &roots[file.archive];
        for (size_t at = 0;;)
        {
            const size_t slash = file.path.find('/', at);
            node = &node->children[file.path.substr(at, slash == std::string::npos ? std::string::npos : slash - at)];
            if (slash == std::string::npos) break;
            Require(!node->file, "overflow path is a file and a folder");
            at = slash + 1;
        }
        Require(!node->file && node->children.empty(), "overflow path twice");
        node->file = &file;
    }

    struct Entry { std::string name; const Node *node; };
    std::vector<std::vector<uint8_t>> records(names.size());
    std::vector<uint32_t> rootCounts(names.size());
    for (size_t a = 0; a < names.size(); ++a)
    {
        Require(sources[a] != UINT32_MAX, "overflow archive without files");
        // Breadth first: the root's entries, then each folder's children together.
        std::vector<Entry> entries;
        for (const auto &[name, child] : roots[a].children) entries.push_back({name, &child});
        rootCounts[a] = uint32_t(entries.size());
        auto &out = records[a];
        for (size_t e = 0; e < entries.size(); ++e)
        {
            const auto [name, node] = entries[e]; // a copy: children are appended below
            std::vector<uint8_t> record(24, 0);
            if (node->file)
            {
                std::copy_n(index.begin() + std::ptrdiff_t(node->file->record), 24, record.begin());
                const uint32_t sector = Le32(index, node->file->record + 8);
                Put32(record, 8, (sector & 0xff000000u) | uint32_t(node->file->offset / kSector));
                Put32(record, 16, node->file->size);
                if (node->file->packed) Put32(record, 20, node->file->decoded);
            }
            else
            {
                const Node *first = node;
                while (!first->file) first = &first->children.begin()->second;
                std::copy_n(index.begin() + std::ptrdiff_t(first->file->record) + 4, 4, record.begin() + 4); // time
                Put32(record, 0, 0x10000000u | bits(name));
                Require(node->children.size() <= 0xffff, "overflow folder too large");
                Put16(record, 14, uint16_t(node->children.size()));
                Put32(record, 20, uint32_t(entries.size()));
                for (const auto &[childName, child] : node->children) entries.push_back({childName, &child});
            }
            out.insert(out.end(), record.begin(), record.end());
        }
    }
    std::vector<uint32_t> archiveNames;
    for (const auto &name : names) archiveNames.push_back(bits(name));

    // New layout after the old data: dictionary, archive table, records.
    std::vector<uint8_t> out(index.begin(), index.end());
    const size_t newDictionary = Align(out.size(), 16);
    out.resize(newDictionary, 0);
    out.insert(out.end(), index.begin() + dictionary, index.begin() + std::ptrdiff_t(dictionary + dictionaryBytes));
    for (const auto word : words) { out.push_back(uint8_t(word)); out.push_back(uint8_t(word >> 8)); }
    out.resize(Align(out.size(), 16), 0);
    const size_t units = (out.size() - newDictionary) / 16;
    Require(units <= 0xffff, "FPI dictionary too large");
    Put16(out, newDictionary + 2, uint16_t(units));

    const size_t newTable = out.size();
    out.resize(newTable + (count + names.size()) * 48, 0);
    auto rebase = [&](size_t from, size_t to, size_t field) {
        const uint32_t rel = Le32(index, from + field);
        if (rel) Put32(out, to + field, uint32_t(from + rel - to));
    };
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t from = table + i * 48, to = newTable + i * 48;
        std::copy_n(index.begin() + std::ptrdiff_t(from), 48, out.begin() + std::ptrdiff_t(to));
        rebase(from, to, 4);
        rebase(from, to, 12);
        Put32(out, to + 8, uint32_t(newDictionary - to));
    }
    for (size_t a = 0; a < names.size(); ++a)
    {
        const size_t from = table + sources[a] * 48, to = newTable + (count + a) * 48;
        std::copy_n(index.begin() + std::ptrdiff_t(from), 48, out.begin() + std::ptrdiff_t(to));
        const size_t base = out.size();
        out.insert(out.end(), records[a].begin(), records[a].end());
        Put16(out, to + 2, uint16_t(rootCounts[a]));
        Put32(out, to + 4, uint32_t(base - to));
        Put32(out, to + 8, uint32_t(newDictionary - to));
        rebase(from, to, 12);
        Put32(out, to + 20, 0); // no folder
        Put32(out, to + 24, (Le32(index, from + 24) & 0xf0000000u) | archiveNames[a]);
    }
    out.resize(Align(out.size(), size_t(kSector)), 0);
    Require(out.size() / kSector <= 512, "FPI index past 1 MiB"); // the loader's limit (sub_82854400)
    Put16(out, 12, uint16_t(out.size() / kSector));
    Put16(out, 26, uint16_t(count + names.size()));
    Put32(out, 32, uint32_t(newTable));
    Put32(out, 40, uint32_t(newDictionary));
    return out;
}
}
