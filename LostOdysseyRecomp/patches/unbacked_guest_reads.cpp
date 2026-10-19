#include <stdafx.h>
#include <kernel/guest_address_space.h>
#include <os/logger.h>
#include <atomic>

// Entering the city of Uhra, sub_82B6E908 reads a byte through the pointer at
// offset 0 of its object (r3), and that pointer leads into physical memory
// the game never allocated (0xA11C40FC on two consoles, two game editions).
// The desktop builds map the whole guest window and read zeros there; the
// Switch backs only allocated pages, so the read faulted. The page behind the
// pointer is now backed with zeros before the original runs, which gives the
// same result as the desktop builds.

extern "C" PPC_FUNC(__imp__sub_82B6E908);

PPC_FUNC(sub_82B6E908)
{
#if LO_PLATFORM_SWITCH
    using GuestAddressSpace::Backing;
    const uint32_t object = ctx.r3.u32;
    if (object >= 0x1000 && GuestAddressSpace::EnsureBacked(object, 32) != Backing::Failed)
    {
        const uint32_t pointer = PPC_LOAD_U32(object);
        static std::atomic<uint32_t> logged{0};
        const auto backing = pointer >= 0x1000 ? GuestAddressSpace::EnsureBacked(pointer, 32) : Backing::AlreadyBacked;
        if (backing != Backing::AlreadyBacked && logged.fetch_add(1, std::memory_order_relaxed) < 8)
            LOG_NOTICE("guest memory: sub_82B6E908 object {:#x} points at unallocated {:#x}: {}", object, pointer,
                       backing == Backing::NewlyBacked ? "backed with zeros, as on PC" : "could not be backed");
    }
#endif
    __imp__sub_82B6E908(ctx, base);
}
