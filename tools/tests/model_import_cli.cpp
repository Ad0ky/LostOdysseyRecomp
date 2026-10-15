// LoModelImport: the model import without the game.
//   LoModelImport --package <decoded .xxx> --export <index> --object <name> --glb <file> --out <file>
//     rebuilds one package with one replacement (modding/model_import.h).
//   LoModelImport --disc <disc folder> --mods <mods folder> --member <archive path> --out <file>
//     takes the runtime path: mod listing, archive overlay (modding/text_overlay.h),
//     then reads the member back through the patched index, archive tail or overflow
//     archive and writes it decoded. --prebuild 1 then prepares the other discs
//     in the background as the runtime does (no delay); --switch <disc> after
//     --switch-after <seconds> opens that disc's index meanwhile, as a disc switch.
#include <gpu/shader/cpx_decode.h>
#include <modding/fpi_index.h>
#include <modding/mod_api.h>
#include <modding/model_import.h>
#include <modding/text_overlay.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
std::vector<uint8_t> ReadFile(const fs::path &file, uint64_t offset = 0, uint64_t size = ~0ull)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + file.string());
    input.seekg(0, std::ios::end);
    const uint64_t length = uint64_t(input.tellg());
    if (size == ~0ull) size = length - offset;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(std::streamoff(offset));
    if (!input.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()))) throw std::runtime_error("short read of " + file.string());
    return bytes;
}

void WriteFile(const fs::path &file, const std::vector<uint8_t> &bytes)
{
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size())))
        throw std::runtime_error("cannot write " + file.string());
}

// A file as the game sees it: the real bytes, with the overlay's ranges on top.
std::vector<uint8_t> Read(const fs::path &file, uint64_t offset, uint64_t size)
{
    const auto ranges = modding::text_overlay::RangesFor(file);
    std::error_code ec;
    const uint64_t real = fs::file_size(file, ec);
    std::vector<uint8_t> bytes(size_t(size), 0);
    if (offset < real)
    {
        const auto part = ReadFile(file, offset, std::min(size, real - offset));
        std::copy(part.begin(), part.end(), bytes.begin());
    }
    for (const auto &range : ranges)
    {
        const uint64_t begin = std::max(offset, range.offset), end = std::min(offset + size, range.offset + range.size);
        if (begin < end && !range.Read(begin - range.offset, bytes.data() + (begin - offset), size_t(end - begin)))
            throw std::runtime_error("cannot read the rebuilt bytes of " + file.string());
    }
    return bytes;
}

int Run(std::map<std::string, std::string> &args)
{
    if (args.count("--package"))
    {
        modding::model_import::Replacement replacement;
        replacement.exportIndex = uint32_t(std::stoul(args.at("--export")));
        replacement.object = args.at("--object");
        replacement.glb = ReadFile(args.at("--glb"));
        replacement.source = args.at("--glb");
        const auto result = modding::model_import::RebuildPackage(ReadFile(args.at("--package")),
                                                                  fs::path(args.at("--package")).stem().string(), {replacement});
        for (const auto &note : result.notes) std::printf("%s\n", note.c_str());
        if (!result.replaced) return 2;
        WriteFile(args.at("--out"), result.package);
        std::printf("wrote %zu bytes\n", result.package.size());
        return 0;
    }
    const fs::path disc = args.at("--disc");
    modding::Initialize(args.at("--mods"));
    if (args.count("--prebuild")) modding::text_overlay::EnablePrebuild(std::chrono::seconds(0));
    std::printf("models listed: %zu\n", modding::ListModels().size());
    const auto header = Read(disc / "LO.fpi", 0, 64);
    const auto index = Read(disc / "LO.fpi", 0, modding::fpi::IndexSize(header));
    const auto parsed = modding::fpi::Read(index);
    if (args.count("--prebuild"))
    {
        const auto began = std::chrono::steady_clock::now();
        auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(); };
        if (args.count("--switch"))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(int(std::stod(args.at("--switch-after")) * 1000)));
            const auto other = disc.parent_path() / ("disc" + args.at("--switch"));
            std::printf("switch to %s at %.1f s\n", other.string().c_str(), seconds());
            Read(other / "LO.fpi", 0, 64);
            std::printf("switch done at %.1f s\n", seconds());
        }
        modding::text_overlay::WaitForPrebuild();
        std::printf("background preparation idle at %.1f s\n", seconds());
    }
    // The record the game opens: an overflow archive's copy comes first.
    const auto *file = modding::fpi::Find(parsed, args.at("--member"));
    if (!file)
    {
        std::fprintf(stderr, "member not found\n");
        return 1;
    }
    const auto archive = disc / parsed.archives.at(file->archive);
    const auto overflow = modding::text_overlay::OverflowArchive(archive);
    auto stored = overflow.empty() ? Read(archive, file->offset, file->size) : ReadFile(overflow, file->offset, file->size);
    std::printf("member in %s at %llu, %u bytes stored\n", parsed.archives.at(file->archive).c_str(),
                static_cast<unsigned long long>(file->offset), file->size);
    std::vector<uint8_t> member = stored;
    if (stored.size() >= 3 && std::memcmp(stored.data(), "cpx", 3) == 0 && !xenos::resources::cpx::Decode(stored, member))
        throw std::runtime_error("invalid CPX data");
    WriteFile(args.at("--out"), member);
    std::printf("wrote %zu bytes\n", member.size());
    return 0;
}
}

int main(int argc, char **argv)
{
    std::map<std::string, std::string> args;
    for (int i = 1; i + 1 < argc; i += 2) args[argv[i]] = argv[i + 1];
    try { return Run(args); }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
