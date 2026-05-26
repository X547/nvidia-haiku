#include "WorkQueue.h"

#include <new>

#include <KernelExport.h>
#include <OS.h>
#include <condition_variable.h>

#include "nv-include.h"
extern "C" {
#include <os-interface.h>
#include <nv.h>
}

#include "Driver.h"


namespace {

// One DPC per queued work item. Allocated on the heap via os_alloc_mem so
// the producer is safe to call from IRQ context (IntrSafePool fallback).
struct NvWorkItem final : DPCCallback {
	void *data;

	void DoDPC(DPCQueue *queue) final;
};


void NvWorkItem::DoDPC(DPCQueue *)
{
	// Match Linux's os_execute_work_item ordering: detach the wrapper before
	// dispatching, so the callback can't see stale wrapper state.
	void *workData = data;
	this->~NvWorkItem();
	os_free_mem(this);

	rm_execute_work_item(nullptr, workData);
}


// Sentinel pushed by os_flush_work_queue. When DoDPC runs we know every
// work item enqueued before us has completed. NotifyOne wakes the caller;
// the sentinel then self-deletes, so the caller's stack never owns it.
struct FlushSentinel final : DPCCallback {
	ConditionVariable *done;

	void DoDPC(DPCQueue *) final
	{
		done->NotifyOne();
		delete this;
	}
};

} // namespace


status_t os_work_queue::Init()
{
	return dpc.Init("nv_work_queue", B_URGENT_DISPLAY_PRIORITY, 0);
}

void os_work_queue::Fini()
{
	// Close(false) drains every pending DPC, then terminates the worker.
	// Any item still in the queue at this point will run normally; if the
	// caller wanted those skipped it should have called os_flush_work_queue
	// with is_unload=true first.
	dpc.Close(false);
}


// #pragma mark - NVIDIA OS interface

extern "C" NV_STATUS NV_API_CALL os_queue_work_item(struct os_work_queue *queue, void *data)
{
	if (queue == nullptr) {
		queue = NvHaikuDriver::Instance().WorkQueue();
		if (queue == nullptr) {
			return NV_ERR_NOT_READY;
		}
	}

	void *mem = nullptr;
	NV_STATUS status = os_alloc_mem(&mem, sizeof(NvWorkItem));
	if (status != NV_OK) {
		return status;
	}

	auto item = new (mem) NvWorkItem();
	item->data = data;
	queue->dpc.Add(item);
	return NV_OK;
}


extern "C" NV_STATUS NV_API_CALL os_flush_work_queue(struct os_work_queue *queue, NvBool is_unload)
{
	if (queue == nullptr) {
		queue = NvHaikuDriver::Instance().WorkQueue();
		if (queue == nullptr) {
			return NV_OK;
		}
	}

	// rm_execute_work_item acquires sleeping locks; calling flush from a
	// context that can't sleep would deadlock waiting on the sentinel.
	if (!are_interrupts_enabled()) {
		return NV_ERR_ILLEGAL_ACTION;
	}

	auto sentinel = new(std::nothrow) FlushSentinel();
	if (sentinel == nullptr) {
		return NV_ERR_NO_MEMORY;
	}

	ConditionVariable done;
	done.Init(sentinel, "nv_flush");
	sentinel->done = &done;

	// Register the wait entry before queueing the sentinel so a notification
	// from the worker thread can never be lost.
	ConditionVariableEntry entry;
	done.Add(&entry);

	queue->isUnloadFlush.store(is_unload != NV_FALSE);
	queue->dpc.Add(sentinel);
	entry.Wait();
	queue->isUnloadFlush.store(false);

	return NV_OK;
}


extern "C" NvBool NV_API_CALL os_is_queue_flush_ongoing(struct os_work_queue *queue)
{
	if (queue == nullptr) {
		queue = NvHaikuDriver::Instance().WorkQueue();
		if (queue == nullptr) {
			return NV_FALSE;
		}
	}
	return queue->isUnloadFlush.load() ? NV_TRUE : NV_FALSE;
}
