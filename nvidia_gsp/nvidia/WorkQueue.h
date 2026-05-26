#pragma once

#include <DPC.h>

#include <atomic>


// Backing for NVIDIA's opaque `struct os_work_queue` (forward-declared in
// os-interface.h). Wraps a Haiku DPCQueue so os_queue_work_item() can defer
// work to a kernel thread, matching Linux's nv_kthread_q.
struct os_work_queue {
	DPCQueue dpc;
	std::atomic<bool> isUnloadFlush {false};

	status_t Init();
	void Fini();
};
