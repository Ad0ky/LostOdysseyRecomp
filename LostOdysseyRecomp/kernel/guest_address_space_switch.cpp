#include <os/platform.h>
#if LO_PLATFORM_SWITCH
#include "guest_address_space_layout.h"
#include <switch.h>
#include <malloc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Nintendo Switch backend (Horizon OS, libnx). Ported from UnleashedRecomp-NX
// (kernel/memory.cpp) and extended with the Xbox 360 physical aliases.
//
// Horizon has no mmap, no shared anonymous memory and no lazy commit. The
// 4 GiB guest window is a virtmem reservation in the ASLR region; guest pages
// are backed explicitly through Commit() by the allocators that hand them out
// (memory.cpp PageAllocator, heap.cpp, image load). A backing chunk is heap
// memory turned into code memory (svcMapProcessCodeMemory) and then mapped
// into the window with svcMapProcessMemory. The same code-memory source can be
// mapped more than once, which gives the physical views their aliases:
//   A  0xA0000000 + x   backing x
//   C  0xC0000000 + x   backing x          (same pages as A)
//   E  0xE0000000 + y   backing y + 0x1000 (A shifted by one 4 KiB page)
// Both syscalls need the homebrew loader to grant them: launch through title
// takeover (hold R while starting a game), not the album applet.
namespace GuestAddressSpace
{
namespace
{
constexpr size_t kPage = 0x1000;
// Commit granularity. Large chunks keep the number of kernel memory blocks
// low; the guest's own allocators are dense, so little is wasted.
constexpr size_t kChunk = 0x200000;
constexpr size_t kChunkCount = kSize / kChunk;
constexpr unsigned kSvcMapProcessMemory = 0x74;
constexpr unsigned kSvcUnmapProcessMemory = 0x75;
constexpr unsigned kSvcMapProcessCodeMemory = 0x77;
constexpr unsigned kSvcUnmapProcessCodeMemory = 0x78;

uint8_t* s_base = nullptr;
VirtmemReservation* s_reservation = nullptr;
Handle s_process = INVALID_HANDLE;
std::mutex s_mutex;
// One flag per 2 MiB chunk of the virtual view and of the A view. C and E are
// committed together with A and never on their own.
bool s_committed[kChunkCount]{};
size_t s_committedBytes = 0;
// Code-memory source of each committed chunk (under s_mutex), to map a view again (EnsureBacked).
uintptr_t s_source[kChunkCount]{};
RepairInfo s_repair{};
// The window on a 2 MiB boundary and full chunks backed 2 MiB aligned, so the kernel maps the virtual, A and C views
// with 2 MiB blocks (one TLB entry each) instead of 512 4 KiB pages. LO_GUEST_BLOCKS=0 (env.txt, applied before the
// static constructors) puts back the 4 KiB layout. The E view is the A view one 4 KiB page later: never a block.
bool s_blocks = true;

bool ProcessMemorySyscallsAvailable()
{
    return envIsSyscallHinted(kSvcMapProcessMemory) && envIsSyscallHinted(kSvcUnmapProcessMemory) &&
        envIsSyscallHinted(kSvcMapProcessCodeMemory) && envIsSyscallHinted(kSvcUnmapProcessCodeMemory);
}

struct HandleReceive
{
    Handle session;
    Handle received;
};

void ReceiveOwnProcessHandle(void* arg)
{
    auto& receive = *static_cast<HandleReceive*>(arg);
    void* tls = armGetTls();
    hipcMakeRequestInline(tls);
    s32 index = 0;
    if (R_SUCCEEDED(svcReplyAndReceive(&index, &receive.session, 1, INVALID_HANDLE, UINT64_MAX)))
    {
        const HipcParsedRequest request = hipcParseRequest(tls);
        if (request.meta.num_copy_handles == 1)
            receive.received = request.data.copy_handles[0];
    }
    svcCloseHandle(receive.session);
}

// The process syscalls need a real handle to this process; they reject the
// CUR_PROCESS_HANDLE pseudo-handle. hbloader passes one. Loaders that do not
// (Ryujinx runs an NRO directly) get one the way hbloader does: send the
// pseudo-handle to ourselves over a session, and the kernel copies a real one.
Handle OwnProcessHandle()
{
    if (const Handle own = envGetOwnProcessHandle(); own != INVALID_HANDLE)
        return own;
    Handle server = INVALID_HANDLE, client = INVALID_HANDLE;
    if (R_FAILED(svcCreateSession(&server, &client, 0, 0)))
        return INVALID_HANDLE;
    HandleReceive receive{ server, INVALID_HANDLE };
    Thread thread;
    if (R_FAILED(threadCreate(&thread, ReceiveOwnProcessHandle, &receive, nullptr, 0x1000, 0x20, -2)) ||
        R_FAILED(threadStart(&thread)))
    {
        svcCloseHandle(server);
        svcCloseHandle(client);
        return INVALID_HANDLE;
    }
    hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1).copy_handles[0] = CUR_PROCESS_HANDLE;
    svcSendSyncRequest(client); // The receiver closes the session instead of replying.
    svcCloseHandle(client);
    threadWaitForExit(&thread);
    threadClose(&thread);
    return receive.received;
}

// Appends the kernel's memory blocks over [address, address + size) to the failure detail:
// "start-end tTYPE aATTR pPERM, ..." (window offsets as +x). Reports whether all of it is free
// and whether a kernel thread-local page is among the blocks.
void AppendBlocks(const char* label, uintptr_t address, size_t size, bool* allFree, bool* threadLocal)
{
    AppendFailureDetail("%s [", label);
    const uintptr_t window = uintptr_t(s_base);
    const uintptr_t end = address + size;
    bool unmapped = true;
    bool first = true;
    for (uintptr_t at = address; at < end;)
    {
        MemoryInfo block{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&block, &pageInfo, at)) || block.size == 0 || block.addr + block.size <= at)
        {
            AppendFailureDetail("%squery failed at %#llx", first ? "" : ", ", (unsigned long long)at);
            unmapped = false;
            break;
        }
        const uintptr_t blockEnd = std::min<uintptr_t>(end, block.addr + block.size);
        const uint32_t type = block.type & MemState_Type;
        if (type != MemType_Unmapped || block.attr != 0)
            unmapped = false;
        if (type == MemType_ThreadLocal && threadLocal)
            *threadLocal = true;
        const bool inside = window && at >= window && at - window < kSize;
        const uintptr_t bias = inside ? window : 0;
        AppendFailureDetail("%s%s%#llx-%#llx t%x a%x p%x", first ? "" : ", ", inside ? "+" : "",
            (unsigned long long)(at - bias), (unsigned long long)(blockEnd - bias), type, block.attr, block.perm);
        first = false;
        at = blockEnd;
    }
    AppendFailureDetail("]");
    if (allFree)
        *allFree = unmapped;
}

// One try at backing [offset, offset + size) of the window with fresh zeroed memory and, for the A view, mapping the
// same pages at the C and E aliases. On failure nothing it mapped is left behind (unless undoing it failed, which the
// detail says), and retry says whether another try with a new source can work.
bool MapChunkOnce(size_t offset, size_t size, uintptr_t& sourceOut, int round, bool& retry)
{
    retry = false;
    // A block needs the backing's physical pages 2 MiB aligned. The heap is mapped with blocks where it can, at
    // addresses congruent to their physical ones modulo 2 MiB, so 2 MiB virtual alignment gives that.
    const bool block = s_blocks && size == kChunk && (offset & (kChunk - 1)) == 0;
    void* backing = memalign(block ? kChunk : kPage, size);
    if (!backing)
    {
        RecordFailure(FailureOperation::CreateBacking, 0, -1, nullptr, size, offset);
        return false;
    }
    std::memset(backing, 0, size);

    // The address is a random free slice, and the mapping can still fail there (a chunk failed about once in 100 runs, at
    // any time, with most of the heap free, and the game then wrote through a null allocation). Another slice works.
    constexpr int kMapAttempts = 16;
    void* source = nullptr;
    Result rc = 0;
    for (int attempt = 0; attempt < kMapAttempts; ++attempt)
    {
        virtmemLock();
        source = virtmemFindCodeMemory(size, kPage);
        bool mapped = false;
        if (source)
        {
            rc = svcMapProcessCodeMemory(s_process, uintptr_t(source), uintptr_t(backing), size);
            mapped = R_SUCCEEDED(rc);
            if (mapped)
            {
                rc = svcSetProcessMemoryPermission(s_process, uintptr_t(source), size, Perm_Rw);
                if (R_FAILED(rc))
                {
                    svcUnmapProcessCodeMemory(s_process, uintptr_t(source), uintptr_t(backing), size);
                    mapped = false;
                }
            }
        }
        virtmemUnlock();
        if (mapped)
            break;
        std::fprintf(stderr, "guest memory: chunk %#zx attempt %d failed at %p, rc %#x\n", offset, attempt, source, rc);
        source = nullptr;
    }
    if (!source)
    {
        RecordFailure(FailureOperation::CreateBacking, rc, -1, source, size, offset);
        AppendFailureDetail("no code-memory source after %d tries (round %d)", kMapAttempts, round + 1);
        free(backing);
        return false;
    }

    struct View
    {
        size_t destination, sourceOffset, length;
    };
    View mapped[3]{};
    int mappedCount = 0;
    auto map = [&](size_t destination, size_t sourceOffset, size_t length, int32_t view) {
        rc = svcMapProcessMemory(s_base + destination, s_process, uintptr_t(source) + sourceOffset, length);
        if (R_SUCCEEDED(rc))
        {
            mapped[mappedCount++] = { destination, sourceOffset, length };
            return true;
        }
        // Usually rc 0xd401 (InvalidCurrentMemory): the destination is not free or the source is not mappable.
        RecordFailure(FailureOperation::MapView, rc, view, s_base + destination, length, offset);
        AppendFailureDetail("round %d: ", round + 1);
        bool destinationFree = false, threadLocal = false;
        AppendBlocks("destination", uintptr_t(s_base + destination), length, &destinationFree, &threadLocal);
        AppendBlocks("; source", uintptr_t(source) + sourceOffset, length, nullptr, nullptr);
        if (threadLocal)
            AppendFailureDetail("; a kernel thread-local page is in the guest window");
        retry = destinationFree;
        return false;
    };

    bool ok = map(offset, 0, size, offset < kStarts[1] ? 0 : 1);
    if (ok && offset >= kStarts[1] && offset < kStarts[2])
    {
        const size_t physical = offset - kStarts[1];
        ok = map(kStarts[2] + physical, 0, size, 2);
        // E: guest E+y reads backing y+0x1000. The first page of the first
        // chunk has no E address (it would fall into C).
        if (ok)
        {
            const size_t skip = physical == 0 ? kPage : 0;
            ok = map(kStarts[3] + physical + skip - kPage, skip, size - skip, 3);
        }
    }
    // Once all views are mapped they are never released: guest memory lives for the whole run.
    if (ok)
    {
        sourceOut = uintptr_t(source);
        return true;
    }

    // Undo this try, so the chunk is either fully mapped or not at all and a later commit can map it.
    // A source still mapped somewhere keeps its backing locked: then neither is released (2 MiB lost, not reused).
    bool released = true;
    for (int view = mappedCount - 1; view >= 0; --view)
    {
        const Result undo = svcUnmapProcessMemory(s_base + mapped[view].destination, s_process,
            uintptr_t(source) + mapped[view].sourceOffset, mapped[view].length);
        if (R_FAILED(undo))
        {
            AppendFailureDetail("; unmapping +%#zx rc %#x: chunk left partly mapped", mapped[view].destination, undo);
            released = false;
        }
    }
    if (released)
    {
        const Result undo = svcUnmapProcessCodeMemory(s_process, uintptr_t(source), uintptr_t(backing), size);
        if (R_SUCCEEDED(undo))
            free(backing);
        else
            AppendFailureDetail("; releasing the source rc %#x: source and backing kept", undo);
    }
    if (!released)
        retry = false; // a view of this chunk is still mapped: another source cannot go there
    return false;
}

// Backs [offset, offset + size) of the window (see MapChunkOnce). When the kernel refused a view although its
// destination was free, the source was the problem: try again with a new one.
bool MapChunk(size_t offset, size_t size, uintptr_t& sourceOut)
{
    constexpr int kRounds = 4;
    for (int round = 0; round < kRounds; ++round)
    {
        bool retry = false;
        if (MapChunkOnce(offset, size, sourceOut, round, retry))
        {
            if (round > 0)
            {
                std::fprintf(stderr, "guest memory: chunk %#zx mapped on round %d after: %s\n", offset, round + 1,
                    GetFailureInfo().detail);
                ClearFailure();
            }
            return true;
        }
        if (!retry)
            return false;
    }
    return false;
}

// Horizon places each thread-local page (TLS of 8 threads, mapped by svcCreateThread when the existing ones are full,
// unmapped when all 8 exit) at a random free address of the process's kernel map region (Atmosphere
// kern_k_page_table_base.cpp: GetRegionAddress(ThreadLocal), MapPages, FindFreeArea). No svcGetInfo reports that
// region, so libnx's virtmemFindAslr can put the guest window inside it, and the kernel knows nothing of virtmem
// reservations: every uncommitted chunk of the window is free memory to it. A thread-local page placed there makes the
// later commit of that chunk fail (svcMapProcessMemory 0xd401) and the game aborts with "out of guest virtual memory".
// Where the region is depends on the address space the title running hbloader was created with (InitializeForProcess):
// - 36-bit (ASLR region 0x8000000-0x1000000000, 64 GiB): the code region, the small map 0x8000000-0x80000000
//   (AddressSmallMap36Start/End). Fixed, 2 GiB.
// - 39-bit (ASLR region 512 GiB): 64 GiB (AddressMemoryRegionSmall39Size) at a random place, apart from the heap, alias
//   and stack regions. Every thread-local page lies inside it, so it lies within kKernelMapSize of them all.
// The window is placed outside that range.
constexpr uint64_t kKernelMapSize = 64ull << 30;
constexpr uintptr_t kSmallMap36Start = 0x8000000;
constexpr uintptr_t kSmallMap36End = 0x80000000;
constexpr uint64_t kAddressSpace36End = 1ull << 36;

PlacementInfo s_placement{};

void FindThreadLocalPages(PlacementInfo& placement)
{
    placement.tlsLow = UINTPTR_MAX;
    placement.tlsHigh = 0;
    uintptr_t at = 0;
    for (int blocks = 0; blocks < 0x10000; ++blocks)
    {
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, at)) || info.size == 0)
            break;
        if ((info.type & MemState_Type) == MemType_ThreadLocal)
        {
            placement.tlsLow = std::min<uintptr_t>(placement.tlsLow, info.addr);
            placement.tlsHigh = std::max<uintptr_t>(placement.tlsHigh, info.addr + info.size);
            placement.tlsPages += uint32_t(info.size / kPage);
        }
        const uintptr_t next = info.addr + info.size;
        if (next <= at)
            break; // the last block reaches the end of the address space
        at = next;
    }
    if (placement.tlsPages == 0)
        placement.tlsLow = 0;
}
}

uint8_t* Allocate()
{
    ClearFailure();
    if (!ProcessMemorySyscallsAvailable())
    {
        // Typical cause: started from the album applet. Logged by main.cpp.
        RecordFailure(FailureOperation::ReserveAny, 0xFFFFFFFFu, -1, nullptr, kSize);
        return nullptr;
    }
    s_process = OwnProcessHandle();
    if (s_process == INVALID_HANDLE)
    {
        RecordFailure(FailureOperation::ReserveAny, 0xFFFFFFFEu, -1, nullptr, kSize);
        return nullptr;
    }

    PlacementInfo placement{};
    u64 aslrStart = 0, aslrSize = 0;
    if (R_SUCCEEDED(svcGetInfo(&aslrStart, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0)) &&
        R_SUCCEEDED(svcGetInfo(&aslrSize, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0)))
    {
        placement.aslrStart = uintptr_t(aslrStart);
        placement.aslrEnd = uintptr_t(aslrStart + aslrSize);
    }
    // The main thread has one, so there is always a page to go by.
    FindThreadLocalPages(placement);
    if (placement.aslrEnd != 0 && placement.aslrEnd <= kAddressSpace36End)
    {
        placement.addressBits = 36;
        placement.avoidStart = kSmallMap36Start;
        placement.avoidEnd = kSmallMap36End;
        // A thread-local page outside it would mean another layout: keep those out too.
        if (placement.tlsPages != 0)
        {
            placement.avoidStart = std::min<uintptr_t>(placement.avoidStart, placement.tlsLow);
            placement.avoidEnd = std::max<uintptr_t>(placement.avoidEnd, placement.tlsHigh);
        }
    }
    else if (placement.tlsPages != 0)
    {
        placement.addressBits = 39;
        placement.avoidStart = std::min<uintptr_t>(placement.tlsLow,
            placement.tlsHigh > kKernelMapSize ? placement.tlsHigh - kKernelMapSize : 0);
        placement.avoidEnd = std::max<uintptr_t>(placement.tlsHigh, placement.tlsLow + kKernelMapSize);
    }

    if (const char* blocks = getenv("LO_GUEST_BLOCKS"); blocks && std::strcmp(blocks, "0") == 0)
        s_blocks = false;
    placement.blocks = s_blocks;
    // virtmemFindAslr's second argument is a guard, not an alignment: ask for 2 MiB more and round the base up.
    const size_t search = s_blocks ? kSize + kChunk : kSize;
    auto find = [&] {
        const uintptr_t found = uintptr_t(virtmemFindAslr(search, kChunk));
        return reinterpret_cast<uint8_t*>(found && s_blocks ? (found + kChunk - 1) & ~uintptr_t(kChunk - 1) : found);
    };

    virtmemLock();
    VirtmemReservation* avoid = placement.avoidEnd > placement.avoidStart
        ? virtmemAddReservation(reinterpret_cast<void*>(placement.avoidStart), placement.avoidEnd - placement.avoidStart)
        : nullptr;
    s_base = find();
    placement.avoided = s_base && avoid;
    if (avoid)
        virtmemRemoveReservation(avoid);
    if (!s_base)
        s_base = find(); // logged: placement.avoided is false
    if (s_base)
        s_reservation = virtmemAddReservation(s_base, kSize);
    virtmemUnlock();
    if (!s_base || !s_reservation)
    {
        RecordFailure(FailureOperation::ReserveAny, 0, -1, s_base, kSize);
        s_base = nullptr;
        return nullptr;
    }
    placement.base = uintptr_t(s_base);
    s_placement = placement;
    // The guest null page stays unmapped: chunk 0 is committed from 0x1000.
    return s_base;
}

PlacementInfo Placement()
{
    return s_placement;
}

void Release(uint8_t* base)
{
    // The process exits with its mappings; nothing to undo early.
    (void)base;
}

bool Commit(uint32_t address, uint32_t size)
{
    if (!s_base || size == 0)
        return s_base != nullptr;
    const uint64_t end = uint64_t(address) + size;
    if (end > kStarts[2])
        return false; // C and E are aliases of A and are committed with it.

    std::lock_guard lock(s_mutex);
    for (uint64_t chunk = address / kChunk; chunk * kChunk < end; ++chunk)
    {
        if (s_committed[chunk])
            continue;
        size_t offset = size_t(chunk * kChunk);
        size_t length = kChunk;
        if (offset == 0)
        {
            // Keep the guest null page unmapped so a null access faults.
            offset = kPage;
            length -= kPage;
        }
        uintptr_t source = 0;
        if (!MapChunk(offset, length, source))
            return false;
        s_source[chunk] = source;
        __atomic_store_n(&s_committed[chunk], true, __ATOMIC_RELEASE); // read without the lock by EnsureBacked
        s_committedBytes += kChunk;
    }
    return true;
}

Backing EnsureBacked(uint32_t address, uint32_t size)
{
    if (!s_base || address < kPage || size == 0)
        return Backing::Failed;
    // C and E alias A: commit the A pages behind them.
    uint64_t start = address;
    if (start >= kStarts[3])
        start = start - kStarts[3] + kStarts[1] + kPage;
    else if (start >= kStarts[2])
        start = start - kStarts[2] + kStarts[1];
    const uint64_t end = start + size;
    if (end > kStarts[2])
        return Backing::Failed;
    bool backed = true;
    for (uint64_t chunk = start / kChunk; chunk * kChunk < end; ++chunk)
        backed = backed && __atomic_load_n(&s_committed[chunk], __ATOMIC_ACQUIRE);
    if (!backed)
        return Commit(uint32_t(start), uint32_t(end - start)) ? Backing::NewlyBacked : Backing::Failed;

    // Committed, so it should be mapped. Entering Uhra, reads of committed chunks still faulted
    // (no backing was needed or failed, yet the page was not there): check with the kernel, and
    // map a missing view again onto the chunk's own source (its C and E aliases keep the data).
    Backing result = Backing::AlreadyBacked;
    for (uint64_t chunk = start / kChunk; chunk * kChunk < end; ++chunk)
    {
        const uint64_t probe = std::max<uint64_t>(start, chunk * kChunk);
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, uintptr_t(s_base + probe))))
            continue;
        if (info.type != MemType_Unmapped && (info.perm & Perm_R) != 0)
            continue;
        // Mapped but lent to an IPC request (an SD card read into guest memory):
        // the kernel takes the pages' permissions until the reply. Wait for it
        // (up to 2 s) instead of mapping over it.
        if (info.type != MemType_Unmapped && (info.attr & MemAttr_IsIpcMapped) != 0)
        {
            for (int wait = 0; wait < 20000 && (info.attr & MemAttr_IsIpcMapped) != 0 && (info.perm & Perm_R) == 0; ++wait)
            {
                svcSleepThread(100'000);
                if (R_FAILED(svcQueryMemory(&info, &pageInfo, uintptr_t(s_base + probe))))
                    break;
            }
            if ((info.perm & Perm_R) != 0)
                continue;
        }

        std::lock_guard lock(s_mutex);
        RepairInfo repair{};
        repair.address = uint32_t(probe);
        repair.type = info.type;
        repair.attribute = info.attr;
        repair.permission = info.perm;
        repair.regionStart = info.addr >= uintptr_t(s_base) ? uint64_t(info.addr - uintptr_t(s_base)) : ~0ull;
        repair.regionSize = info.size;
        // The same physical page through C, for comparison.
        if (probe >= kStarts[1] && probe < kStarts[2])
        {
            MemoryInfo alias{};
            if (R_SUCCEEDED(svcQueryMemory(&alias, &pageInfo, uintptr_t(s_base + probe - kStarts[1] + kStarts[2]))))
            {
                repair.aliasType = alias.type;
                repair.aliasPermission = alias.perm;
            }
        }
        // Where the chunk's view starts in the window and in its source.
        const uint64_t chunkStart = chunk * kChunk;
        const uint64_t mapStart = chunkStart == 0 ? kPage : chunkStart;
        const uint64_t chunkEnd = chunkStart + kChunk;
        const uintptr_t source = s_source[chunk];
        repair.source = source;
        if (source)
        {
            MemoryInfo backing{};
            if (R_SUCCEEDED(svcQueryMemory(&backing, &pageInfo, source)))
            {
                repair.sourceType = backing.type;
                repair.sourcePermission = backing.perm;
            }
        }
        // The chunk's memory blocks as the kernel sees them now.
        size_t length = 0;
        for (uint64_t at = mapStart; at < chunkEnd && length + 48 < sizeof(repair.layout);)
        {
            MemoryInfo block{};
            if (R_FAILED(svcQueryMemory(&block, &pageInfo, uintptr_t(s_base + at))) || block.size == 0)
                break;
            const uint64_t blockEnd = std::min<uint64_t>(chunkEnd, uint64_t(block.addr + block.size - uintptr_t(s_base)));
            const int written = std::snprintf(repair.layout + length, sizeof(repair.layout) - length, "%s%#llx-%#llx t%x p%x",
                length ? ", " : "", (unsigned long long)at, (unsigned long long)blockEnd, block.type, block.perm);
            if (written <= 0)
                break;
            length += size_t(written);
            at = blockEnd;
        }
        // Map only the unmapped block, within this chunk, onto the matching part of the source.
        const uint64_t blockStart = std::max<uint64_t>(mapStart, uint64_t(info.addr - uintptr_t(s_base)));
        const uint64_t blockEnd = std::min<uint64_t>(chunkEnd, uint64_t(info.addr + info.size - uintptr_t(s_base)));
        repair.mappedStart = blockStart;
        repair.mappedSize = blockEnd > blockStart ? blockEnd - blockStart : 0;
        repair.result = source && blockEnd > blockStart
            ? svcMapProcessMemory(s_base + blockStart, s_process, source + (blockStart - mapStart), blockEnd - blockStart)
            : 0xFFFFFFFFu;
        repair.count = s_repair.count + 1;
        s_repair = repair;
        if (R_FAILED(repair.result))
            return Backing::Failed;
        result = Backing::Repaired;
    }
    return result;
}

namespace
{
// LatencyTest: the cost of a TLB miss, measured on the console. The same number of cache lines is read in a random
// order twice: one line per page over thousands of pages (each load needs its own translation), then 64 lines per page
// over a few pages (translations hit). Both sets fit the 2 MiB L2 cache, so the difference is the page walk. A walk
// over 4 KiB pages misses far more often than one over 2 MiB blocks, which one TLB entry covers. svcQueryMemory does
// not report the page size, so this is how to tell which one the kernel used.
constexpr size_t kChaseLines = 2048;

// Which 64-byte line of the n-th page. It must look random: physically contiguous pages index the L2's sets with
// address bits 6-16, and a regular pattern piles the lines into a few sets that then miss the cache.
size_t LineOfPage(size_t n)
{
    return size_t((uint64_t(n + 1) * 0x9E3779B97F4A7C15ull) >> 58);
}

void Shuffle(uintptr_t* items, size_t count)
{
    uint64_t x = 0x2545F4914F6CDD1Dull;
    for (size_t i = count; i > 1; --i)
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        std::swap(items[i - 1], items[x % i]);
    }
}

// Nanoseconds per load, best of 8 passes after a warm-up. Each address depends on the previous load's value, so the
// loads cannot overlap.
double LoadNs(const uintptr_t* addresses, size_t count)
{
    uint64_t best = UINT64_MAX;
    uint64_t chain = 0;
    for (int pass = 0; pass < 9; ++pass)
    {
        const uint64_t start = armGetSystemTick();
        for (size_t i = 0; i < count; ++i)
        {
            uint64_t value;
            __asm__ volatile("ldr %[v], [%[a], %[c]]\n\tand %[c], %[v], xzr"
                : [v] "=&r"(value), [c] "+r"(chain) : [a] "r"(addresses[i]) : "memory");
        }
        const uint64_t ticks = armGetSystemTick() - start;
        if (pass > 0 && ticks < best)
            best = ticks;
    }
    return double(armTicksToNs(best)) / double(count);
}

// The same for instruction fetches: each address is a ret, so the call only fetches it.
double FetchNs(const uintptr_t* addresses, size_t count)
{
    uint64_t best = UINT64_MAX;
    for (int pass = 0; pass < 9; ++pass)
    {
        const uint64_t start = armGetSystemTick();
        for (size_t i = 0; i < count; ++i)
            reinterpret_cast<void (*)()>(addresses[i])();
        const uint64_t ticks = armGetSystemTick() - start;
        if (pass > 0 && ticks < best)
            best = ticks;
    }
    return double(armTicksToNs(best)) / double(count);
}

void EmitPair(void (*emit)(const char*), const char* what, double scattered, double grouped, size_t pages)
{
    char line[192];
    std::snprintf(line, sizeof(line), "tlb: %s: %.1f ns a load over %zu pages, %.1f ns over 32 pages: +%.1f ns a miss",
        what, scattered, pages, grouped, scattered - grouped);
    emit(line);
}

bool Readable(uintptr_t address, uintptr_t& blockEnd)
{
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, address)) || info.size == 0)
    {
        blockEnd = address + kPage;
        return false;
    }
    blockEnd = info.addr + info.size;
    return (info.type & MemState_Type) != MemType_Unmapped && (info.perm & Perm_R) != 0 &&
        (info.attr & MemAttr_IsIpcMapped) == 0;
}

// A ret in the 64-byte line at address, or 0.
uintptr_t RetInLine(uintptr_t line)
{
    constexpr uint32_t kRet = 0xD65F03C0;
    for (uintptr_t at = line; at < line + 64; at += 4)
        if (*reinterpret_cast<const uint32_t*>(at) == kRet)
            return at;
    return 0;
}

void CodeTest(void (*emit)(const char*), const MemoryInfo& block, const char* what, bool fetch)
{
    static uintptr_t scattered[kChaseLines], grouped[kChaseLines];
    size_t ns = 0, ng = 0;
    const size_t pages = block.size / kPage;
    if (pages < 2 * 32)
        return;
    const size_t stride = std::max<size_t>(1, pages / kChaseLines);
    for (size_t i = 0; i < kChaseLines && i * stride < pages; ++i)
    {
        const uintptr_t page = block.addr + i * stride * kPage;
        const uintptr_t line = page + LineOfPage(i) * 64;
        if (!fetch)
            scattered[ns++] = line;
        else
            for (uintptr_t at = line; at < page + kPage; at += 64)
                if (const uintptr_t ret = RetInLine(at)) { scattered[ns++] = ret; break; }
    }
    const uintptr_t middle = block.addr + (pages / 2) * kPage;
    for (uintptr_t line = middle; line < middle + 32 * kPage && ng < kChaseLines; line += 64)
    {
        if (!fetch)
            grouped[ng++] = line;
        else if (const uintptr_t ret = RetInLine(line))
            grouped[ng++] = ret;
    }
    if (ns < kChaseLines / 2 || ng < 256)
        return;
    Shuffle(scattered, ns);
    Shuffle(grouped, ng);
    const double far = fetch ? FetchNs(scattered, ns) : LoadNs(scattered, ns);
    const double near = fetch ? FetchNs(grouped, ng) : LoadNs(grouped, ng);
    EmitPair(emit, what, far, near, ns);
}
}

void LatencyTest(void (*emit)(const char*))
{
    char line[192];
    std::snprintf(line, sizeof(line), "tlb: window at %p, %s a 2 MiB boundary, blocks %s", s_base,
        (uintptr_t(s_base) & (kChunk - 1)) ? "not on" : "on", s_blocks ? "on" : "off (LO_GUEST_BLOCKS=0)");
    emit(line);
    if (!s_base)
        return;
    static uintptr_t scattered[kChaseLines], grouped[kChaseLines];
    static const char* const kNames[] = { "guest virtual view", "guest A view", "guest C view (A alias)",
        "guest E view (A + 4 KiB)" };
    for (int view = 0; view < 4; ++view)
    {
        size_t ns = 0, ng = 0;
        double far = 0, near = 0;
        {
            // Committed chunks stay mapped; the lock only keeps the commit state still while it is read.
            std::lock_guard lock(s_mutex);
            const size_t viewEnd = view == 0 ? kStarts[1] : kStarts[view] + kSizes[view];
            for (size_t w = kStarts[view]; w < viewEnd && ns < kChaseLines; w += kPage)
            {
                size_t backingOffset = w;
                if (view == 2)
                    backingOffset = w - kStarts[2] + kStarts[1];
                else if (view == 3)
                    backingOffset = w - kStarts[3] + kStarts[1] + kPage;
                if (backingOffset < kPage) // the guest null page
                    continue;
                if (backingOffset >= kStarts[2]) // E's last page: past the end of A
                    break;
                if (!s_committed[backingOffset / kChunk])
                {
                    w += kChunk - backingOffset % kChunk - kPage; // to where the next chunk starts
                    continue;
                }
                uintptr_t blockEnd = 0;
                if (!Readable(uintptr_t(s_base + w), blockEnd))
                    continue;
                const uintptr_t page = uintptr_t(s_base + w);
                scattered[ns] = page + LineOfPage(ns) * 64;
                ++ns;
                for (size_t l = 0; l < 64 && ng < kChaseLines; ++l)
                    grouped[ng++] = page + l * 64;
            }
            if (ns < kChaseLines)
                continue; // too little of this view is in use
            Shuffle(scattered, ns);
            Shuffle(grouped, ng);
            far = LoadNs(scattered, ns);
            near = LoadNs(grouped, ng);
        }
        EmitPair(emit, kNames[view], far, near, ns);
    }

    // The NRO's code and read-only data (hbloader maps them with 4 KiB pages unless they line up).
    MemoryInfo text{}, rodata{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&text, &pageInfo, uintptr_t(&LatencyTest))))
        return;
    CodeTest(emit, text, "NRO code, read", false);
    CodeTest(emit, text, "NRO code, run", true);
    if (R_SUCCEEDED(svcQueryMemory(&rodata, &pageInfo, text.addr + text.size)) && (rodata.perm & Perm_R))
        CodeTest(emit, rodata, "NRO read-only data", false);
}

RepairInfo LastRepair()
{
    std::lock_guard lock(s_mutex);
    return s_repair;
}

size_t CommittedBytes()
{
    std::lock_guard lock(s_mutex);
    return s_committedBytes;
}
}
#endif
