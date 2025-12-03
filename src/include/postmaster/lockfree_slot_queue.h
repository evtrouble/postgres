#ifndef LOCKFREE_SLOT_QUEUE_H
#define LOCKFREE_SLOT_QUEUE_H

#include "postgres.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "utils/timestamp.h"
#include "postmaster/postmaster.h"  /* for PMChild */

/*
 * Lock-free queue for idle backend PMChild pointers with timestamps.
 * - Multi-producer (backends returning PMChild* pointers)
 * - Single-consumer (postmaster assigning slots)
 * - Capacity equals MaxBackends, fixed at init time.
 * - max_pool_size can be reduced at runtime (but not increased beyond capacity).
 */

/*
 * Entry in the queue: PMChild pointer and when it became idle.
 * 
 * Note: PMChild* is stored as uintptr_t (address value) because:
 * - Backend processes (producers) only write the pointer value, never use it
 * - Postmaster (consumer) reads the pointer value and uses it in its own address space
 * - The pointer value is just an integer/address that gets passed through
 */
typedef struct PoolSlotEntry {
    uintptr_t   pmchild_ptr;   /* PMChild* as address value (0 means empty) */
    TimestampTz idle_since;     /* when this backend became idle */
} PoolSlotEntry;

typedef struct LockFreeSlotQueue {
    pg_atomic_uint32 head;      /* next position to dequeue (consumer) */
    pg_atomic_uint32 tail;      /* next position to enqueue (producer) */
    uint32          capacity;   /* queue capacity (equals MaxBackends) */
    PoolSlotEntry  *entries;    /* shared memory array of slot entries */
} LockFreeSlotQueue;

/*
 * Initialize the queue in shared memory.
 * 'capacity' equals MaxBackends.
 * Returns true on success.
 */
extern bool LockFreeSlotQueueInit(LockFreeSlotQueue *queue, uint32 capacity, PoolSlotEntry *shared_entries);

/*
 * Enqueue a PMChild pointer with timestamp (called by backend process).
 * Returns true if enqueued, false if:
 *   - queue full
 *   - pmchild_ptr is 0 (NULL)
 * 
 * Note: max_pool_size check is done in PoolGetIdleBackend() when postmaster
 * dequeues, because backend processes cannot access PMChild* in postmaster's
 * private memory.
 */
extern bool LockFreeSlotQueueEnqueue(LockFreeSlotQueue *queue,
                                     uintptr_t pmchild_ptr,
                                     TimestampTz idle_since,
                                     uint32 current_max_pool_size);

/*
 * Peek at the head entry without dequeuing (called by postmaster only).
 * Returns PMChild* pointer on success, NULL if empty.
 * If 'idle_since' is not NULL, stores the timestamp when the slot became idle.
 * 
 * This is used for checking expiration without removing the entry from queue.
 */
extern PMChild *LockFreeSlotQueuePeek(LockFreeSlotQueue *queue, TimestampTz *idle_since);

/*
 * Dequeue a slot entry (called by postmaster only).
 * Returns PMChild* pointer on success, NULL if empty.
 * If 'idle_since' is not NULL, stores the timestamp when the slot became idle.
 */
extern PMChild *LockFreeSlotQueueDequeue(LockFreeSlotQueue *queue, TimestampTz *idle_since);

#endif
