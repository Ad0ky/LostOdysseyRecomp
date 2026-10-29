// The debug server on a computer, for testing the protocol, tools/switch/lo_debug.py and tools/scenario.py --switch
// without a console. The server core and the PNG writer are the Switch build's own files
// (LostOdysseyRecomp/os/switch/debug_server_core.cpp, ppm_png.cpp); the game's commands (debug_commands.cpp, which
// needs libnx and the game) are stand-ins here with the same names, arguments and replies, and the log is a made-up
// game's: a line per frame with the timing lines tools/scenario.py reads, draws rising once the scene "loads".
//
// Build and run from the repository root (macOS or Linux):
//   M=tools/XenosRecomp/thirdparty/smol-v/testing/external/miniz; S=LostOdysseyRecomp/os/switch
//   cc -O1 -c $M/miniz.c -o out/miniz.o
//   c++ -std=c++20 -O1 -I$S -I$M tools/switch/debug_server_host_test.cpp $S/debug_server_core.cpp $S/ppm_png.cpp \
//       out/miniz.o -o out/debug_server_host_test
//   out/debug_server_host_test out/fake-sd 6543 &      # an empty folder plays the app folder; 127.0.0.1 only
//   python3 tools/switch/lo_debug.py --host 127.0.0.1 info
// Options after the port: frames per second of the fake game (default 120), and the frame its scene loads at (300).
#include "debug_server_core.h"
#include "ppm_png.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace
{
std::string g_root;
int g_port = 6543;
char** g_argv = nullptr;
uint64_t g_session = 0;
std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_inputs{0};

// The network log's history (os/switch/netlog.cpp): the last 1-2 MiB, Read as it does.
std::mutex g_logMutex;
std::string g_history;
uint64_t g_end = 0;

void Log(const std::string& line)
{
    std::lock_guard lock(g_logMutex);
    g_history += line + "\n";
    g_end += line.size() + 1;
    if (g_history.size() > 2u << 20)
        g_history.erase(0, g_history.size() - (1u << 20));
}

uint64_t Read(uint64_t& position, std::string& text)
{
    std::lock_guard lock(g_logMutex);
    const uint64_t first = g_end - g_history.size();
    const uint64_t dropped = position < first ? first - position : 0;
    const uint64_t from = std::max(position, first);
    text.append(g_history, size_t(std::min(from, g_end) - first), std::string::npos);
    position = g_end;
    return dropped;
}

double Seconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void Game(int fps, uint32_t sceneFrame)
{
    for (uint32_t frame = 1;; frame++)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(1000000 / fps));
        g_frame = frame;
        const uint32_t draws = frame < sceneFrame ? 40 : 900 + frame % 17;
        char line[256];
        std::snprintf(line, sizeof(line), "[%9.3f t0001] [info]  render timing frame=%u draws=%u gpu_queue_batches_elapsed_ms=%.2f",
            Seconds(), frame, draws, 12.0 + frame % 5);
        Log(line);
        std::snprintf(line, sizeof(line), "[%9.3f t0001] [info]  present timing completed=%u target=30 frame_ms=%.1f", Seconds(),
            frame, frame % 50 ? 33.3 : 40.0);
        Log(line);
    }
}

// A 320x180 gradient with the frame number in its brightness, as video::SaveScreenshot would save it.
bool SavePpm(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const unsigned w = 320, h = 180, frame = g_frame.load();
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
        {
            const unsigned char rgb[3] = { (unsigned char)(x * 255 / w), (unsigned char)(y * 255 / h), (unsigned char)(frame & 0xff) };
            std::fwrite(rgb, 1, 3, f);
        }
    return std::fclose(f) == 0;
}

debugsrv::Reply Info(const debugsrv::Args&)
{
    char text[512];
    std::snprintf(text, sizeof(text),
        "build host-test (debug_server_host_test)\nsession %016llx (up %.0f s)\nframe %u\nmap -\nmode handheld\n"
        "clocks cpu 1020 MHz, gpu 460 MHz, memory 1331 MHz\nmemory (host test)\nbattery unknown\n"
        "program 0500000000000000 (restart works: HOME-menu forwarder)\naddress 127.0.0.1:%d\n",
        (unsigned long long)g_session, Seconds(), g_frame.load(), g_port);
    return debugsrv::ok(text);
}

debugsrv::Reply Shot(const debugsrv::Args& a)
{
    const std::string path = g_root + "/debug-shot-test.ppm";
    if (!SavePpm(path))
        return debugsrv::err("cannot save " + path);
    Log("[debug] screenshot at frame " + std::to_string(g_frame.load()));
    if (!a.empty() && a[0] == "ppm")
    {
        debugsrv::Reply r;
        r.file = path;
        r.removeFile = true;
        return r;
    }
    std::string png, why;
    const bool converted = os::debug_server::PpmToPng(path, png, why);
    std::remove(path.c_str());
    return converted ? debugsrv::ok(std::move(png)) : debugsrv::err(why);
}

debugsrv::Reply Record(const char* name, const debugsrv::Args& a)
{
    std::string line = std::string("[debug] ") + name;
    for (const auto& arg : a)
        line += " " + arg;
    Log(line);
    g_inputs++;
    return debugsrv::ok();
}
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <folder> [port] [fps] [scene frame]\n", argv[0]);
        return 2;
    }
    g_argv = argv;
    g_root = argv[1];
    g_port = argc > 2 ? std::atoi(argv[2]) : 6543;
    const int fps = argc > 3 ? std::atoi(argv[3]) : 120;
    const uint32_t sceneFrame = argc > 4 ? uint32_t(std::atoi(argv[4])) : 300;
    g_session = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    debugsrv::add_command("info", "info                   (host test) build, session, frame", Info);
    debugsrv::add_command("shot", "shot [ppm]             (host test) a gradient", Shot);
    for (const char* name : { "input", "press", "hold", "release", "stick" })
        debugsrv::add_command(name, (std::string(name) + "                  (host test) logged").c_str(),
            [name](const debugsrv::Args& a) { return Record(name, a); });
    debugsrv::add_command("restart", "restart [force]        (host test) the program starts again", [](const debugsrv::Args&) {
        debugsrv::Reply r = debugsrv::ok("restarting: the program starts again");
        r.after = [] {
            debugsrv::stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            execv(g_argv[0], g_argv);
            std::perror("execv");
            std::_Exit(1);
        };
        return r;
    });
    debugsrv::Config config;
    config.port = g_port;
    config.root = g_root;
    config.absolutePrefix = "";
    config.loopbackOnly = true; // this computer only
    config.log = [](const char* line) { std::fprintf(stderr, "%s\n", line); Log(line); };
    config.readLog = Read;
    Log("[debug] host test session starting");
    for (int attempt = 0; !debugsrv::start(config); attempt++)
    {
        if (attempt == 50)
            return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the previous image's socket may linger
    }
    std::fprintf(stderr, "[debug] host test server on port %d, folder %s\n", g_port, g_root.c_str());
    Game(fps, sceneFrame);
}
