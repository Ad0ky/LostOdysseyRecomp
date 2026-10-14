// LoFpiOverflowTest: overflow archives added to a small made-up FPI index
// (modding/fpi_overflow.h). No game data.
#include <modding/fpi_overflow.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace fpi = modding::fpi;

namespace
{
using Bytes = std::vector<uint8_t>;

uint32_t Le32(const Bytes &b, size_t p) { return fpi::detail::Le32(b, p); }

// lo.fpd (no folder): rpgame/a.ini. xenon_field.fpd (bin\field): m1/m1_map.xxx, m1/m1_mapw.xxx.
Bytes MakeIndex()
{
    // Dictionary: marker, size in 16-byte units, then the names.
    std::vector<uint16_t> dictionary{1, 0};
    auto word = [&](const char *name) {
        const auto index = uint32_t(dictionary.size());
        for (const auto w : fpi::detail::Encode(name)) dictionary.push_back(w);
        return index;
    };
    const uint32_t xxx = word("XXX"), ini = word("INI"), rpgame = word("RPGAME"), a = word("A"), lo = word("LO.FPD"),
                   field = word("XENON_FIELD.FPD"), prefix = word("BIN\\FIELD"), m1 = word("M1"), map = word("M1_MAP"),
                   mapw = word("M1_MAPW");
    while (dictionary.size() % 8) dictionary.push_back(0);
    dictionary[1] = uint16_t(dictionary.size() / 8);

    Bytes b(64, 0);
    auto put16 = [&](size_t p, uint16_t v) { fpi::detail::Put16(b, p, v); };
    auto put32 = [&](size_t p, uint32_t v) { fpi::detail::Put32(b, p, v); };
    const size_t table = 64, extensions = table + 2 * 48, records0 = extensions + 16, records1 = records0 + 2 * 24,
                 dict = records1 + 3 * 24;
    b.resize(dict + dictionary.size() * 2, 0);
    for (size_t i = 0; i < dictionary.size(); ++i) put16(dict + 2 * i, dictionary[i]);
    put32(8, 0x10000);
    b[20] = 1; // disc
    b[23] = 2; // extensions
    put16(24, 1);
    put16(26, 2);
    put32(32, uint32_t(table));
    put32(40, uint32_t(dict));
    put32(44, uint32_t(extensions));
    put16(extensions, uint16_t(xxx));
    put16(extensions + 2, uint16_t(ini));
    auto archive = [&](size_t i, uint32_t name, uint32_t folder, size_t records) {
        const size_t ar = table + i * 48;
        b[ar] = 1;
        put16(ar + 2, 1);
        put32(ar + 4, uint32_t(records - ar));
        put32(ar + 8, uint32_t(dict - ar));
        put32(ar + 12, uint32_t(extensions - ar));
        put32(ar + 20, folder ? 0x10000000u | folder : 0);
        put32(ar + 24, 0x10000000u | name);
        put32(ar + 32, i ? 0x40000000u : 0);
    };
    archive(0, lo, 0, records0);
    archive(1, field, prefix, records1);
    auto folder = [&](size_t p, uint32_t name, uint16_t children, uint32_t first) {
        put32(p, 0x10000000u | name);
        put16(p + 14, children);
        put32(p + 20, first);
    };
    auto file = [&](size_t p, uint32_t name, uint32_t ext, uint32_t sector, uint32_t size) {
        put32(p, name | ext << 23);
        put32(p + 4, 0x0fd30000);
        put32(p + 8, 0x08000000u | sector);
        put16(p + 14, 0x40);
        put32(p + 16, size);
        put32(p + 20, size * 3);
    };
    folder(records0, rpgame, 1, 1);
    file(records0 + 24, a, 2, 0, 100);
    folder(records1, m1, 2, 1);
    file(records1 + 24, map, 1, 10, 3000);
    file(records1 + 48, mapw, 1, 12, 500);
    b.resize((b.size() + fpi::kSector - 1) / fpi::kSector * fpi::kSector, 0);
    put16(12, uint16_t(b.size() / fpi::kSector));
    return b;
}

const fpi::File &Only(const fpi::Index &index, const std::string &path, uint32_t archive)
{
    const fpi::File *found = nullptr;
    for (const auto &f : index.files)
        if (f.path == path && f.archive == archive) found = &f;
    assert(found);
    return *found;
}

// What the game needs of every archive record: its dictionary and extensions.
void CheckArchives(const Bytes &b)
{
    const uint32_t table = Le32(b, 32), dict = Le32(b, 40), ext = Le32(b, 44);
    for (uint32_t i = 0; i < fpi::detail::Le16(b, 26); ++i)
    {
        const size_t ar = table + i * 48;
        assert(uint32_t(ar + Le32(b, ar + 8)) == dict); // 32-bit, as the game adds them
        assert(uint32_t(ar + Le32(b, ar + 12)) == ext);
    }
    assert(fpi::detail::Le16(b, 12) * fpi::kSector == b.size());
    assert(fpi::detail::Le16(b, dict + 2) * 16 + dict <= b.size());
}
}

int main()
{
    const auto original = MakeIndex();
    const auto before = fpi::Read(original);
    assert(before.archives.size() == 2 && before.archives[1] == "xenon_field.fpd");
    assert(before.prefixes[0].empty() && before.prefixes[1] == "bin/field");
    const auto &map = Only(before, "bin/field/m1/m1_map.xxx", 1);
    assert(map.offset == 10 * fpi::kSector && map.size == 3000);
    CheckArchives(original);

    // One file into one overflow archive.
    fpi::OverflowFile moved{map.record, map.archive, map.path, 0, 4 * fpi::kSector, 9000, 27000, true};
    const auto one = fpi::AddOverflowArchives(original, {"xenon_mod0.fpd"}, {moved});
    CheckArchives(one);
    const auto after = fpi::Read(one);
    assert(after.archives.size() == 3 && after.archives[2] == "xenon_mod0.fpd" && after.prefixes[2].empty());
    const auto &copy = Only(after, "bin/field/m1/m1_map.xxx", 2);
    assert(copy.offset == 4 * fpi::kSector && copy.size == 9000);
    assert(Le32(one, copy.record + 20) == 27000);
    assert(Le32(one, copy.record + 8) >> 24 == 0x08); // disc and layer byte kept
    assert(Le32(one, copy.record + 4) == Le32(original, map.record + 4));
    assert(fpi::Find(after, "bin/field/m1/m1_map.xxx") == &copy);
    // Untouched: the original record, the other files.
    assert(Only(after, "bin/field/m1/m1_map.xxx", 1).offset == map.offset);
    assert(fpi::Find(after, "bin/field/m1/m1_mapw.xxx")->archive == 1);
    assert(fpi::Find(after, "rpgame/a.ini")->archive == 0);
    assert(std::equal(original.begin() + 64, original.end(), one.begin() + 64)); // only the header changes in place
    // The overflow archive copies its source's flags.
    assert(Le32(one, Le32(one, 32) + 2 * 48 + 32) == 0x40000000u);

    // Two archives, a folder name the dictionary lacks, two files in one folder.
    const auto &mapw = Only(before, "bin/field/m1/m1_mapw.xxx", 1);
    const auto &ini = Only(before, "rpgame/a.ini", 0);
    std::vector<fpi::OverflowFile> files{
        {map.record, 1, "bin/field/m1/m1_map.xxx", 0, 0, 100, 300, true},
        {mapw.record, 1, "bin/field/m1/m1_mapw.xxx", 0, fpi::kSector, 200, 600, true},
        {ini.record, 0, "extra/deeper/a.ini", 1, 0, 50, 0, false},
    };
    const auto two = fpi::AddOverflowArchives(original, {"xenon_mod0.fpd", "xenon_mod1.fpd"}, files);
    CheckArchives(two);
    const auto parsed = fpi::Read(two);
    assert(parsed.archives.size() == 4 && parsed.archives[3] == "xenon_mod1.fpd");
    assert(fpi::Find(parsed, "bin/field/m1/m1_mapw.xxx")->archive == 2);
    assert(fpi::Find(parsed, "bin/field/m1/m1_mapw.xxx")->offset == fpi::kSector);
    assert(fpi::Find(parsed, "extra/deeper/a.ini")->archive == 3);
    assert(Le32(two, fpi::Find(parsed, "extra/deeper/a.ini")->record + 20) == Le32(original, ini.record + 20)); // not CPX

    // Refused: past 4 GiB, a path twice, a name the dictionary cannot hold.
    auto throws = [&](std::vector<fpi::OverflowFile> bad, std::vector<std::string> names) {
        try { fpi::AddOverflowArchives(original, names, bad); }
        catch (const std::runtime_error &) { return true; }
        return false;
    };
    assert(throws({{map.record, 1, map.path, 0, uint64_t(1) << 32, 10, 30, true}}, {"xenon_mod0.fpd"}));
    assert(throws({moved, moved}, {"xenon_mod0.fpd"}));
    assert(throws({moved}, {"xenon mod.fpd"}));
    std::puts("LoFpiOverflowTest passed");
    return 0;
}
