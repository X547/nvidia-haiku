// NvRmUserMemoryMapDemo
//
// Probe: what happens when an NVRM memory object created from a userland
// pointer (an OS descriptor, via NvRmApi::AllocOsDescriptor) is CPU-mapped
// back through the RM map path (NV_ESC_RM_MAP_MEMORY + NV_HAIKU_MAP), at
// offset 0 and at a non-zero page offset?
//
// Why this is interesting:
//
//   * The kernel system-memory map path (BaseDevice.cpp NV_HAIKU_MAP,
//     control-device branch) assumes the memory object's OS-private data is an
//     nv_alloc_t — a driver-allocated Haiku `area` it can vm_clone_area().
//   * An OS descriptor instead stores an nv_user_pages_t token (the kernel's
//     clone of the caller's B_FULL_LOCK area): the user pages are already
//     mapped in the calling process, so NV_HAIKU_MAP refuses them; both
//     tokens carry their kind, so neither is taken for the other.
//
// So this is a type-mismatch probe, not a missing-implementation probe. None
// of the functions on this path (nv_add_mapping_context_to_file,
// nv_alloc_user_mapping, the NV_HAIKU_MAP handler) are panic stubs in
// stubs.c — they are all implemented. The relevant point about stubs.c is the
// inverse: if a *panic* stub were reached, panic() halts the whole kernel, so
// this demo printing any result at all is itself evidence that no panic stub
// was hit. What we expect to see instead is either an RM-level error from
// NV_ESC_RM_MAP_MEMORY or NV_HAIKU_MAP refusing the token.
//
// This is exactly why the VK_EXT_external_memory_host path returns the
// application's original pointer from vkMapMemory rather than round-tripping
// through NV_ESC_RM_MAP_MEMORY.

#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <system_error>

#include <OS.h>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
}

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>


int main()
{
	void *hostPtr = nullptr;
	area_id hostArea = -1;
	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		const NvU64 pageSize = B_PAGE_SIZE;
		const NvU64 size = 2 * pageSize;

		// OS descriptors take only memory in a B_FULL_LOCK area.
		hostArea = create_area("user memory map demo", &hostPtr, B_ANY_ADDRESS, size,
			B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (hostArea < B_OK) {
			throw std::system_error(ENOMEM, std::generic_category(), "create_area");
		}
		auto hostBytes = (NvU8*)hostPtr;
		for (NvU64 i = 0; i < size; i++) {
			hostBytes[i] = (NvU8)(i * 7 + 0x11);
		}

		printf("host buffer:   %p (%" PRIu64 " bytes, %" PRIu64 " pages)\n",
			hostPtr, size, size / pageSize);

		{
			// Import the user pages as an OS-descriptor memory object. Freed
			// when osDesc leaves scope (which releases the kernel's clone),
			// before we delete the host area below.
			NvRmObject osDesc(rm, rm.AllocOsDescriptor(rmDev.Device().Get(), hostPtr, size, /*writable=*/true));
			printf("os descriptor: %#" PRIx32 "\n\n", osDesc.Get());

			// Attempt a CPU map of the OS-descriptor object and report the
			// outcome. A throw means the map was rejected (RM status or a
			// NV_HAIKU_MAP failure); a return means it mapped, in which case we
			// check whether the mapped bytes actually alias the host pattern.
			auto probe = [&](const char *label, bool sysMem, NvU64 offset, NvU64 length) {
				printf("%s (offset=%#" PRIx64 ", len=%#" PRIx64 "):\n", label, offset, length);
				try {
					NvRmMemoryMapping m = rmDev.MapMemory(osDesc.Get(), sysMem, offset, length, 0);
					auto mapped = (const NvU8*)m.Address();
					bool aliases = (mapped != nullptr) &&
						memcmp(mapped, hostBytes + offset, length) == 0;
					printf("  -> MAPPED at %p, aliases host pattern: %s\n",
						mapped, aliases ? "yes" : "NO (mapped unrelated memory)");
				} catch (const std::system_error &ex) {
					printf("  -> rejected: %s\n", ex.what());
				}
				puts("");
			};

			// System-memory branch: routes through the control device, where
			// NV_HAIKU_MAP treats the priv-data as nv_alloc_t.
			probe("sysmem map", true, 0, pageSize);
			probe("sysmem map", true, pageSize, pageSize);

			// Device branch: routes through the per-GPU node, where the map is
			// built from the physical ranges RM filled in (no nv_alloc_t cast).
			probe("device map", false, 0, pageSize);
		}

		delete_area(hostArea);
		hostArea = -1;

		puts("reached end without a kernel panic: no stubs.c panic stub was hit");
		return 0;
	} catch (const std::system_error &ex) {
		if (hostArea >= B_OK) {
			delete_area(hostArea);
		}
		fprintf(stderr, "[!] NvRmUserMemoryMapDemo: %s\n", ex.what());
		return 1;
	}
}
