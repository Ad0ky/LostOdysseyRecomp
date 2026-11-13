// The NRO's code and read-only data on 2 MiB blocks (LO_CODE_BLOCKS, on unless it is 0).
//
// hbloader reads the NRO to the start of the heap and maps it with svcMapProcessCodeMemory at a random address of the
// code region. Virtual and physical addresses then differ by an amount that is not a multiple of 2 MiB, and the
// kernel has to map the 55 MiB of .text and 34 MiB of .rodata with 4 KiB pages. LO_TLB_TEST measured a load that
// misses the TLB there at +55 to +111 ns (4 KiB pages in the guest window: +25 to +45 ns, blocks: about 0).
//
// Here the same bytes are copied, before any C++ static initializer runs, to a heap range that lines up with the code
// modulo 2 MiB (the heap is mapped with the largest blocks first, so its virtual and physical addresses line up), and
// that copy is mapped at the same virtual address in place of the loader's. No pointer moves and nothing else changes.
// The method is renut's (sdk/src/ui/switch_code_blocks.cpp, from kameo-nx): this is a separate implementation that
// also gives the loader's mapping back on the way out instead of never returning to the loader.
//
// What keeps it safe:
//  - The code cannot unmap itself, so the swap runs from a page of its own (SwapStub, copied out of .rodata to a
//    code page) and uses only registers and the stack, which stay mapped.
//  - Nothing else runs the code during the swap: priority 102 runs right after env_file.cpp's ApplyEarly (101) and
//    before every other constructor. No thread of ours exists yet, and libnx's __appInit starts none.
//  - .text and .rodata are read-only: crt0's relocation pass wrote only to the RW segment (.data, the GOT), which
//    stays where it is, so a copy made now is the same as the original.
//  - svcUnmapProcessCodeMemory fails and changes nothing unless it is given the pages that are mapped there. A wrong
//    guess at where the loader's copy is costs nothing: the code stays where it was.
//  - The stub maps the old copy back if the new one is refused. If even that fails nothing could run: brk.
//  - hbloader unmaps the NRO from its own copy when main returns or the process calls exit or _Exit, and stops with
//    a fatal error if the pages there are not its own. __libnx_exit, which every one of those exits ends in, is
//    replaced below: it swaps the loader's copy back first (same bytes, so the code keeps running). ExitNow and the
//    debug restart end the process in the kernel and never return to the loader.
//
// The cost: the loader's copy cannot be given back (it is below the heap the loader hands over, with .data after
// it), so .text and .rodata take their size again: about 89 MiB of the 3 GiB heap.
#include <os/platform.h>
#if LO_PLATFORM_SWITCH
#include <switch.h>
#include <malloc.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C"
{
// The first byte of .text (crt0's _start, which hbloader maps first). Not __start__, an absolute symbol the
// relocation step never moves.
extern char _start;
// libnx's runtime: __appExit tears the services down, __nx_exit returns to the loader (switch_crt0.s).
void __appExit(void);
void NX_NORETURN __nx_exit(Result rc, LoaderReturnFn retaddr);
// The swap (SwapStub below): its bytes, copied to a page of their own.
extern const uint32_t lo_code_swap_begin[];
extern const uint32_t lo_code_swap_end[];
}

namespace
{
constexpr uint64_t kBlock = 0x200000;
constexpr uint64_t kPage = 0x1000;

// The layout SwapStub reads.
struct Segment
{
    uint64_t va;     // where the code is mapped
    uint64_t oldSrc; // the heap pages mapped there now
    uint64_t newSrc; // the heap pages to map there instead
    uint64_t size;
    uint64_t perm;
};
struct SwapArgs
{
    uint32_t process;
    uint32_t count;
    uint32_t done; // written by the stub: segments swapped, from the first
    uint32_t pad;
    Segment seg[2];
};
static_assert(sizeof(Segment) == 40 && offsetof(SwapArgs, done) == 8 && offsetof(SwapArgs, seg) == 16,
    "SwapStub reads this layout");

// uint32_t SwapStub(SwapArgs*): for each segment in turn, unmap the old copy (svc 0x78 UnmapProcessCodeMemory), map
// the new one (0x77 MapProcessCodeMemory) and set its permission (0x73 SetProcessMemoryPermission). Returns 0, or the
// kernel's result for the segment that stopped it; done says how many were swapped. A segment that fails after its
// old copy was unmapped gets the old copy back, so every segment is mapped when it returns; if putting it back fails
// too, no code can run: brk. Assembled here and copied to a code page; it must not refer to anything outside itself.
// x19 handle, x20 segments left, x21 segment, x22 result, x23 args. A svc keeps x19-x30.
__asm__(
    ".pushsection .rodata.lo_code_swap, \"a\"\n"
    ".balign 16\n"
    ".global lo_code_swap_begin\n"
    ".hidden lo_code_swap_begin\n"
    ".global lo_code_swap_end\n"
    ".hidden lo_code_swap_end\n"
    "lo_code_swap_begin:\n"
    "    stp x19, x20, [sp, #-48]!\n"
    "    stp x21, x22, [sp, #16]\n"
    "    str x23, [sp, #32]\n"
    "    mov x23, x0\n"
    "    ldr w19, [x23]\n"
    "    ldr w20, [x23, #4]\n"
    "    str wzr, [x23, #8]\n"
    "    add x21, x23, #16\n"
    "1:  cbz w20, 9f\n"
    "    mov w0, w19\n"
    "    ldp x1, x2, [x21]\n"        // va, old copy
    "    ldr x3, [x21, #24]\n"
    "    svc #0x78\n"                // unmap the old copy
    "    cbnz w0, 8f\n"              // refused: this segment is as it was
    "    mov w0, w19\n"
    "    ldr x1, [x21]\n"
    "    ldr x2, [x21, #16]\n"       // new copy
    "    ldr x3, [x21, #24]\n"
    "    svc #0x77\n"                // map the new copy
    "    mov w22, w0\n"
    "    cbnz w0, 5f\n"
    "    mov w0, w19\n"
    "    ldr x1, [x21]\n"
    "    ldr x2, [x21, #24]\n"
    "    ldr x3, [x21, #32]\n"
    "    svc #0x73\n"                // its permission
    "    mov w22, w0\n"
    "    cbz w0, 7f\n"
    "    mov w0, w19\n"              // refused: unmap the new copy again
    "    ldr x1, [x21]\n"
    "    ldr x2, [x21, #16]\n"
    "    ldr x3, [x21, #24]\n"
    "    svc #0x78\n"
    "    cbnz w0, 6f\n"
    "5:  mov w0, w19\n"              // put the old copy back
    "    ldp x1, x2, [x21]\n"
    "    ldr x3, [x21, #24]\n"
    "    svc #0x77\n"
    "    cbnz w0, 6f\n"
    "    mov w0, w19\n"
    "    ldr x1, [x21]\n"
    "    ldr x2, [x21, #24]\n"
    "    ldr x3, [x21, #32]\n"
    "    svc #0x73\n"
    "    cbnz w0, 6f\n"
    "    mov w0, w22\n"
    "    b 8f\n"
    "7:  ldr w9, [x23, #8]\n"        // swapped: the next segment
    "    add w9, w9, #1\n"
    "    str w9, [x23, #8]\n"
    "    add x21, x21, #40\n"
    "    sub w20, w20, #1\n"
    "    b 1b\n"
    "9:  mov w0, #0\n"
    "8:  ldr x23, [sp, #32]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x19, x20, [sp], #48\n"
    "    ret\n"
    "6:  brk #0\n"
    "lo_code_swap_end:\n"
    ".popsection\n");

char g_status[256] = "code blocks: did not run";
Handle g_process = INVALID_HANDLE;
uint64_t g_stub = 0;       // the stub's code page, kept mapped for the exit
void* g_stubHeap = nullptr;
SwapArgs g_swapped{};      // what the start-up swap did, for the exit to undo

using SwapFn = uint32_t (*)(SwapArgs*);

// One code page holding the stub, at a free address of the code region.
bool MapStub(Handle process)
{
    const size_t bytes = size_t(reinterpret_cast<const char*>(lo_code_swap_end) -
                                reinterpret_cast<const char*>(lo_code_swap_begin));
    void* page = memalign(kPage, kPage);
    if (!page || bytes > kPage)
    {
        free(page);
        return false;
    }
    std::memset(page, 0, kPage);
    std::memcpy(page, lo_code_swap_begin, bytes);
    armDCacheFlush(page, kPage);
    virtmemLock();
    void* va = virtmemFindCodeMemory(kPage, kPage);
    Result rc = va ? svcMapProcessCodeMemory(process, uint64_t(va), uint64_t(page), kPage) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_SUCCEEDED(rc))
    {
        rc = svcSetProcessMemoryPermission(process, uint64_t(va), kPage, Perm_Rx);
        if (R_FAILED(rc))
            svcUnmapProcessCodeMemory(process, uint64_t(va), uint64_t(page), kPage);
    }
    if (R_FAILED(rc))
    {
        free(page);
        return false;
    }
    armICacheInvalidate(va, kPage);
    g_stub = uint64_t(va);
    g_stubHeap = page;
    return true;
}

void UnmapStub()
{
    svcUnmapProcessCodeMemory(g_process, g_stub, uint64_t(g_stubHeap), kPage);
    free(g_stubHeap);
    g_stub = 0;
    g_stubHeap = nullptr;
}

uint64_t Blocks(uint64_t va, uint64_t size)
{
    const uint64_t low = (va + kBlock - 1) & ~(kBlock - 1);
    const uint64_t high = (va + size) & ~(kBlock - 1);
    return high > low ? (high - low) / kBlock : 0;
}

void Apply()
{
    const auto skip = [](const char* why, uint64_t value) {
        std::snprintf(g_status, sizeof(g_status), "code blocks: left on 4 KiB pages, %s (%#llx)", why,
            static_cast<unsigned long long>(value));
    };
    if (const char* setting = getenv("LO_CODE_BLOCKS"); setting && std::strcmp(setting, "0") == 0)
    {
        std::snprintf(g_status, sizeof(g_status), "code blocks: off (LO_CODE_BLOCKS=0)");
        return;
    }
    const Handle process = envGetOwnProcessHandle();
    if (process == INVALID_HANDLE)
        return skip("no process handle from the loader", 0);

    const uint64_t textVa = uint64_t(&_start);
    MemoryInfo text{}, rodata{}, loader{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&text, &pageInfo, textVa)) || text.addr != textVa || text.perm != Perm_Rx)
        return skip("the code is not one executable block at _start", text.addr);
    const uint64_t rodataVa = textVa + text.size;
    if (R_FAILED(svcQueryMemory(&rodata, &pageInfo, rodataVa)) || rodata.addr != rodataVa || rodata.perm != Perm_R)
        return skip("no read-only block after the code", rodata.perm);
    const uint64_t total = text.size + rodata.size;

    // The loader's copy: the start of the heap, lent to the kernel by svcMapProcessCodeMemory.
    uint64_t heapBase = 0;
    if (R_FAILED(svcGetInfo(&heapBase, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcQueryMemory(&loader, &pageInfo, heapBase)) || loader.addr != heapBase ||
        !(loader.attr & MemAttr_IsBorrowed) || loader.size < total)
        return skip("the loader's copy is not at the start of the heap", loader.attr);
    if (((textVa - heapBase) & (kBlock - 1)) == 0)
    {
        std::snprintf(g_status, sizeof(g_status), "code blocks: the loader's copy already lines up with 2 MiB blocks");
        return;
    }

    // The copy lines up with the code modulo 2 MiB. Whole pages are lent to the kernel: the alignment keeps the first
    // clear of this allocation's header and the extra page keeps the last clear of the next one's. Kept for the
    // process's life (the exit gives the loader its pages back but leaves this allocation alone).
    void* raw = memalign(kBlock, total + kBlock + kPage);
    if (!raw)
        return skip("no memory for the copy", total);
    const uint64_t copy = uint64_t(raw) + ((textVa - uint64_t(raw)) & (kBlock - 1));
    std::memcpy(reinterpret_cast<void*>(copy), reinterpret_cast<const void*>(textVa), total);
    armDCacheFlush(reinterpret_cast<void*>(copy), total);

    if (!MapStub(process))
    {
        free(raw);
        return skip("no code page for the swap", 0);
    }
    g_process = process;
    SwapArgs args{};
    args.process = process;
    args.count = 2;
    args.seg[0] = {textVa, heapBase, copy, text.size, Perm_Rx};
    args.seg[1] = {rodataVa, heapBase + text.size, copy + text.size, rodata.size, Perm_R};
    const uint32_t rc = reinterpret_cast<SwapFn>(g_stub)(&args);
    g_swapped = args;
    if (args.done == 0)
    {
        UnmapStub();
        free(raw);
        return skip("the kernel refused the swap", rc);
    }
    armICacheInvalidate(reinterpret_cast<void*>(textVa), text.size);
    std::snprintf(g_status, sizeof(g_status),
        "code blocks: %s on a copy in line with 2 MiB blocks (%llu and %llu blocks; code %#llx-%#llx, copy %#llx, "
        "%llu MiB more heap)%s",
        args.done == 2 ? "code and read-only data" : "code only", static_cast<unsigned long long>(Blocks(textVa, text.size)),
        static_cast<unsigned long long>(args.done == 2 ? Blocks(rodataVa, rodata.size) : 0),
        static_cast<unsigned long long>(textVa), static_cast<unsigned long long>(textVa + total),
        static_cast<unsigned long long>(copy), static_cast<unsigned long long>((total + kBlock + kPage) >> 20),
        args.done == 2 ? "" : ", the read-only data was refused");
}

__attribute__((constructor(102))) void CodeBlocksInit()
{
    Apply();
}
} // namespace

// Every return to the loader (main returning, exit, _Exit) ends here: libnx's own __libnx_exit, a weak symbol, with
// the loader's copy of the code put back before it returns. hbloader unmaps the NRO from that copy, and stops with
// a fatal error if the pages mapped there are not its own.
extern "C" void NX_NORETURN __libnx_exit(int rc)
{
    (void)rc;
    __appExit();
    if (g_swapped.done > 0)
    {
        SwapArgs back{};
        back.process = g_process;
        back.count = g_swapped.done;
        for (uint32_t i = 0; i < g_swapped.done; ++i)
        {
            const Segment& s = g_swapped.seg[i];
            back.seg[i] = {s.va, s.newSrc, s.oldSrc, s.size, s.perm};
        }
        reinterpret_cast<SwapFn>(g_stub)(&back);
        // The loader could not take the NRO back: end the process here instead of in the loader's fatal error.
        if (back.done != back.count)
            svcExitProcess();
        // The loader hands the heap to the next NRO: leave no page of it lent to the kernel.
        UnmapStub();
    }
    __nx_exit(0, envGetExitFuncPtr());
}

namespace os::switch_platform
{
const char* CodeBlocksStatus()
{
    return g_status;
}
} // namespace os::switch_platform
#endif
