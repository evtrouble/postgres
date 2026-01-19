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

    pg_atomic_init_u64(&queue->head, 0);
    pg_atomic_init_u64(&queue->tail, 0);
    queue->capacity = capacity;
    queue->entries = shared_entries;

    /* Clear the entries array (pmchild_ptr = 0 means empty) */
    for (uint32 i = 0; i < capacity; i++)
    {
        pg_atomic_init_u64(&shared_entries[i].seq, i);
        pg_atomic_init_u64(&shared_entries[i].pmchild_ptr, 0);
        shared_entries[i].idle_since = 0;
    }

    return true;
}

bool
LockFreeSlotQueueEnqueue(LockFreeSlotQueue *queue,
                         uintptr_t pmchild_ptr,
                         TimestampTz idle_since,
                         uint32 current_max_pool_size)
{
    uint64 pos;

    if (queue == NULL || queue->entries == NULL)
        return false;

    if (pmchild_ptr == 0)
        return false;

    (void) current_max_pool_size;

    for (;;)
    {
        PoolSlotEntry *cell;
        uint64 seq;
        int64 dif;

        pos = pg_atomic_read_u64(&queue->tail);
        cell = &queue->entries[pos % queue->capacity];
        seq = pg_atomic_read_u64(&cell->seq);
        dif = (int64) seq - (int64) pos;

        if (dif == 0)
        {
            if (pg_atomic_compare_exchange_u64(&queue->tail, &pos, pos + 1))
                break;
        }
        else if (dif < 0)
        {
            return false;
        }

        pg_spin_delay();
    }

    {
        PoolSlotEntry *cell = &queue->entries[pos % queue->capacity];

        cell->idle_since = idle_since;
        pg_atomic_write_u64(&cell->pmchild_ptr, (uint64) pmchild_ptr);
        pg_atomic_write_membarrier_u64(&cell->seq, pos + 1);
    }

    return true;
}

PMChild *
LockFreeSlotQueuePeek(LockFreeSlotQueue *queue, TimestampTz *idle_since)
{
    uint64 pos;
    PoolSlotEntry *cell;
    uint64 seq;
    int64 dif;
    uint64 pmchild_ptr;

    if (queue == NULL || queue->entries == NULL)
        return NULL;

    pos = pg_atomic_read_u64(&queue->head);
    cell = &queue->entries[pos % queue->capacity];
    seq = pg_atomic_read_membarrier_u64(&cell->seq);
    dif = (int64) seq - (int64) (pos + 1);

    if (dif != 0)
        return NULL;
    
    /* Return timestamp if requested */
    if (idle_since != NULL)
        *idle_since = cell->idle_since;

    pmchild_ptr = pg_atomic_read_u64(&cell->pmchild_ptr);

    return (PMChild *) ((uintptr_t) pmchild_ptr);
}

PMChild *
LockFreeSlotQueueDequeue(LockFreeSlotQueue *queue, TimestampTz *idle_since)
{
    uint64 pos;
    PoolSlotEntry *cell;
    uint64 seq;
    int64 dif;
    uint64 pmchild_ptr;
    TimestampTz enqueue_since;

    if (queue == NULL || queue->entries == NULL)
        return NULL;

    for (;;)
    {
        pos = pg_atomic_read_u64(&queue->head);
        cell = &queue->entries[pos % queue->capacity];
		seq = pg_atomic_read_membarrier_u64(&cell->seq);
        dif = (int64) seq - (int64) (pos + 1);

        if (dif == 0)
        {
            if (pg_atomic_compare_exchange_u64(&queue->head, &pos, pos + 1))
                break;
        }
        else if (dif < 0)
        {
            return NULL;
        }

        pg_spin_delay();
    }

    enqueue_since = cell->idle_since;

    /* Return timestamp if requested */
    if (idle_since != NULL)
        *idle_since = enqueue_since;

    pmchild_ptr = pg_atomic_read_u64(&cell->pmchild_ptr);

    cell->idle_since = 0;
    pg_atomic_write_u64(&cell->pmchild_ptr, 0);
    pg_atomic_write_membarrier_u64(&cell->seq, pos + queue->capacity);

    return (PMChild *) ((uintptr_t) pmchild_ptr);
}
