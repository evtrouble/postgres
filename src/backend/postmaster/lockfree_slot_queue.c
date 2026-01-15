#include "postmaster/lockfree_slot_queue.h"
#include "utils/elog.h"

bool
LockFreeSlotQueueInit(LockFreeSlotQueue *queue, uint32 capacity, PoolSlotEntry *shared_entries)
{
    /* Validate capacity is reasonable */
    if (capacity == 0 || capacity > 65536) /* safety limit */
    {
        elog(WARNING, "invalid queue capacity: %u", capacity);
        return false;
    }

    if (queue == NULL || shared_entries == NULL)
        return false;

    pg_atomic_init_u32(&queue->head, 0);
    pg_atomic_init_u32(&queue->tail, 0);
    queue->capacity = capacity;
    queue->entries = shared_entries;

    /* Clear the entries array (pmchild_ptr = 0 means empty) */
    MemSet(shared_entries, 0, sizeof(PoolSlotEntry) * capacity);

    return true;
}

bool
LockFreeSlotQueueEnqueue(LockFreeSlotQueue *queue,
                         uintptr_t pmchild_ptr,
                         TimestampTz idle_since,
                         uint32 current_max_pool_size)
{
    uint32 tail, next_tail, head;

    if (queue == NULL || queue->entries == NULL)
        return false;

    if (pmchild_ptr == 0)
        return false;

    /* Note: We cannot check child_slot here because PMChild* points to
     * postmaster's private memory, which backend processes cannot access.
     * The check is done in PoolGetIdleBackend() when postmaster dequeues.
     */

    for (;;)
    {
        tail = pg_atomic_read_u32(&queue->tail);
        head = pg_atomic_read_u32(&queue->head);

        next_tail = tail + 1;
        if(next_tail >= queue->capacity)
            next_tail = 0;

        /* Check if full (one slot always left unused) */
        if (next_tail == head)
        {
            elog(DEBUG2, "slot queue is full (pmchild_ptr=%p)", (void *)pmchild_ptr);
            return false;
        }

        /* Try to claim the tail slot */
        if (pg_atomic_compare_exchange_u32(&queue->tail, &tail, next_tail))
        {
            /*
             * Successfully claimed position 'tail'.
             * Write the entry (pmchild_ptr and timestamp) atomically.
             * pg_write_barrier() ensures visibility to consumer.
             */
            queue->entries[tail].pmchild_ptr = pmchild_ptr;
            queue->entries[tail].idle_since = idle_since;
            pg_write_barrier(); /* release semantics */

            elog(DEBUG3, "enqueued PMChild* %p at index %u (idle_since=%ld)",
                 (void *)pmchild_ptr, tail, (long) idle_since);
            return true;
        }

        /* CAS failed due to contention, retry */
        pg_spin_delay();
    }
}

PMChild *
LockFreeSlotQueuePeek(LockFreeSlotQueue *queue, TimestampTz *idle_since)
{
    uint32 head, tail;
    uintptr_t pmchild_ptr;
    PMChild *pmchild;

    if (queue == NULL || queue->entries == NULL)
        return NULL;

    head = pg_atomic_read_u32(&queue->head);
    tail = pg_atomic_read_u32(&queue->tail);

    /* Check if empty */
    if (head == tail)
        return NULL;

    /*
     * Single consumer: we own 'head', no need for CAS.
     * Read the entry (pmchild_ptr and timestamp) without removing it.
     */
    pg_read_barrier(); /* acquire semantics */
    pmchild_ptr = queue->entries[head].pmchild_ptr;
    
    /* Return timestamp if requested */
    if (idle_since != NULL)
        *idle_since = queue->entries[head].idle_since;

    /* Convert uintptr_t back to PMChild* */
    pmchild = (PMChild *) pmchild_ptr;

    elog(DEBUG3, "peeked PMChild* %p from index %u (idle_since=%ld)",
         (void *)pmchild_ptr, head, idle_since ? (long) *idle_since : 0);
    return pmchild;
}

PMChild *
LockFreeSlotQueueDequeue(LockFreeSlotQueue *queue, TimestampTz *idle_since)
{
    uint32 head, tail;
    uintptr_t pmchild_ptr;
    PMChild *pmchild;
    TimestampTz enqueue_since;

    if (queue == NULL || queue->entries == NULL)
        return NULL;

    head = pg_atomic_read_u32(&queue->head);
    tail = pg_atomic_read_u32(&queue->tail);

    /* Check if empty */
    if (head == tail)
        return NULL;

    /*
     * Single consumer: we own 'head', no need for CAS.
     * Read the entry (pmchild_ptr and timestamp).
     */
    pg_read_barrier(); /* acquire semantics */
    pmchild_ptr = queue->entries[head].pmchild_ptr;
    enqueue_since = queue->entries[head].idle_since;
    
    /* Return timestamp if requested */
    if (idle_since != NULL)
        *idle_since = queue->entries[head].idle_since;

    /* Clear the entry for reuse */
    queue->entries[head].pmchild_ptr = 0;
    queue->entries[head].idle_since = 0;

    /* Advance head */
    {
        uint32 next_head = head + 1;
        if(next_head >= queue->capacity)
            next_head = 0;
        pg_atomic_write_u32(&queue->head, next_head);
    }

    /* Convert uintptr_t back to PMChild* */
    pmchild = (PMChild *) pmchild_ptr;

    elog(DEBUG3, "dequeued PMChild* %p from index %u (idle_since=%ld)",
         (void *)pmchild_ptr, head, (long) enqueue_since);
    return pmchild;
}
