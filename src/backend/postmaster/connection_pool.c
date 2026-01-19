/*-------------------------------------------------------------------------
 *
 * connection_pool.c
 *	  Lock-free connection pool implementation for PostgreSQL
 *
 * This file implements a lock-free connection pool that manages backend
 * processes for reuse. The pool uses a lock-free queue stored in shared
 * memory to coordinate between multiple backend processes (producers) and
 * the postmaster's accept loop (single consumer).
 *
 * DESIGN NOTES:
 * =============
 *
 * 1. Lock-Free Queue Design:
 *    - Queue stores PMChild* pointers (as uintptr_t address values)
 *    - PMChild structures are allocated in postmaster process
 *    - Backend processes receive PMChild* at startup and store it as uintptr_t
 *    - Multiple backend processes (producers) enqueue their PMChild* when
 *      they finish serving a client
 *    - Single consumer (postmaster accept loop) dequeues PMChild* directly,
 *      no lookup needed
 *
 * 2. Concurrency Model:
 *    - Multiple Producers: Backend processes enqueue PMChild* atomically
 *    - Single Consumer: Postmaster accept loop dequeues PMChild*
 *    - Uses atomic operations (pg_atomic_*) for lock-free synchronization
 *    - Queue is implemented as a circular buffer in shared memory
 *
 * 3. Pool Sizing:
 *    - Pool can be shrunk: expired backends are removed by PoolCleanupExpired()
 *      which peeks the queue head, accumulates timeout counts, and removes
 *      backends after multiple consecutive timeout checks
 *    - Queue capacity equals MaxBackends, fixed at startup
 *    - NOTE: MaxConnections cannot be increased dynamically (PGC_POSTMASTER)
 *      - Increasing MaxConnections requires a server restart
 *
 * 4. Integration Points:
 *    - Backend processes receive PMChild* in BackendStartupData at startup
 *    - Backend processes call PoolEnqueuePMChild() when finishing a connection
 *    - Postmaster calls PoolGetIdleBackend() in ServerLoop() to get idle backends
 *    - No lookup needed: PMChild* is returned directly from queue
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/connection_pool.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>

#include "postmaster/connection_pool.h"
#include "postmaster/lockfree_slot_queue.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "port/atomics.h"
#include "utils/timestamp.h"
#include "miscadmin.h"

/*
 * Connection pool shared memory structure
 */
typedef struct ConnectionPoolShmem
{
	LockFreeSlotQueue *queue;		/* Lock-free queue for slot entries */
	int			max_pool_size;		/* Maximum pool size (from GUC) */
	int			current_pool_size;	/* Current number of backends in pool */
	int			idle_timeout;		/* Idle timeout in seconds (from GUC) */
	int			min_idle_size;		/* Minimum idle backends to keep */
} ConnectionPoolShmem;

/* Pointer to shared memory structure */
static ConnectionPoolShmem *pool_shmem = NULL;

/* Timeout tracking for dynamic pool shrinking (postmaster process only, not shared) */
static int timeout_count = 0;				/* Consecutive timeout checks for queue head */

/* GUC variables - accessed directly */
extern int connection_pool_size;
extern int connection_pool_idle_timeout;
extern int connection_pool_min_idle_size;

/*
 * Calculate shared memory size needed for connection pool
 *
 * NOTE: Queue capacity equals MaxBackends
 */
Size
ConnectionPoolShmemSize(void)
{
	Size		size;
	Size		queue_size;
	uint32		queue_capacity;
	
	/* Size of ConnectionPoolShmem structure */
	size = sizeof(ConnectionPoolShmem);
	
	/* Queue capacity equals MaxBackends
	 * MaxBackends should be initialized by InitializeMaxBackends() before
	 * this function is called.
	 */
	if (MaxBackends <= 0)
		elog(ERROR, "MaxBackends not initialized");
	
	/* Use MaxBackends directly as queue capacity */
	queue_capacity = (uint32) MaxBackends;
	
	/* Size of LockFreeSlotQueue structure */
	queue_size = sizeof(LockFreeSlotQueue);
	size = add_size(size, queue_size);
	
	/* Size of queue entries array */
	queue_size = queue_capacity * sizeof(PoolSlotEntry);
	size = add_size(size, queue_size);
	return size;
}

/*
 * Initialize connection pool shared memory
 */
void
ConnectionPoolShmemInit(void)
{
	bool		found;
	bool		queue_found;
	bool		entries_found;
	Size		queue_size;
	Size		entries_size;
	uint32		queue_capacity;
	
	/* Allocate shared memory structure */
	pool_shmem = (ConnectionPoolShmem *)
		ShmemInitStruct("Connection Pool", sizeof(ConnectionPoolShmem), &found);
	
	if (!found)
	{
		/* First time initialization */
		pool_shmem->max_pool_size = connection_pool_size;
		pool_shmem->current_pool_size = 0;
		pool_shmem->idle_timeout = connection_pool_idle_timeout;
		pool_shmem->min_idle_size = connection_pool_min_idle_size;
	}
	
	/* Calculate queue capacity using the same formula as ConnectionPoolShmemSize()
	 * This ensures consistency between size calculation and initialization
	 * Queue capacity equals MaxBackends
	 */
	if (MaxBackends <= 0)
		elog(ERROR, "MaxBackends not initialized");
	
	/* Use MaxBackends directly as queue capacity */
	queue_capacity = (uint32) MaxBackends;
	
	/* Allocate queue structure in shared memory */
	queue_size = sizeof(LockFreeSlotQueue);
	pool_shmem->queue = (LockFreeSlotQueue *)
		ShmemInitStruct("Connection Pool Queue", queue_size, &queue_found);
	
	/* Allocate queue entries array in shared memory */
	{
		PoolSlotEntry *queue_entries;
		
		entries_size = queue_capacity * sizeof(PoolSlotEntry);
		queue_entries = (PoolSlotEntry *)
			ShmemInitStruct("Connection Pool Queue Entries", entries_size, &entries_found);
		
		if (!queue_found && !entries_found)
		{
			/* Initialize queue */
			if (!LockFreeSlotQueueInit(pool_shmem->queue, queue_capacity, queue_entries))
			{
				elog(ERROR, "failed to initialize connection pool queue");
			}
		}
		else
		{
			/* Queue already exists, verify capacity matches */
			if (pool_shmem->queue->capacity != queue_capacity)
				elog(WARNING, "connection pool queue capacity mismatch: expected %u, got %u",
					 queue_capacity, pool_shmem->queue->capacity);
		}
	}
}

/*
 * Initialize the connection pool (called from postmaster)
 */
void
InitConnectionPool(void)
{
	/* Shared memory should already be initialized */
	if (pool_shmem == NULL)
		elog(ERROR, "connection pool shared memory not initialized");
	
	elog(DEBUG1, "connection pool initialized (max_size=%d, idle_timeout=%d)",
		 pool_shmem->max_pool_size, pool_shmem->idle_timeout);
}

/*
 * Set pool configuration from GUC variables
 * Called when GUC variables are reloaded (SIGHUP)
 */
void
ConfigureConnectionPool(void)
{
	/* Update shared memory with current GUC values */
	if (pool_shmem != NULL)
	{
		pool_shmem->max_pool_size = connection_pool_size;
		pool_shmem->idle_timeout = connection_pool_idle_timeout;
		pool_shmem->min_idle_size = connection_pool_min_idle_size;
	}
	
	elog(DEBUG1, "connection pool configured (max_size=%d, idle_timeout=%d)",
		 connection_pool_size, connection_pool_idle_timeout);
}


/*
 * Enqueue PMChild pointer to the lock-free queue
 *
 * This is called by backend processes when they finish serving a client
 * and want to return to the pool. This is the producer side (multiple
 * backends can call this concurrently).
 *
 * 'pmchild_ptr' is the PMChild* pointer passed from postmaster at startup,
 * stored as uintptr_t (address value). Backend processes only store this
 * value, never use it.
 *
 * Returns true if successfully enqueued, false if queue is full or
 * pmchild_ptr is invalid (0).
 * 
 * Note: max_pool_size check is done in PoolGetIdleBackend() when postmaster
 * dequeues, because backend processes cannot access PMChild* in postmaster's
 * private memory.
 *
 * THREAD SAFETY: Lock-free, can be called from multiple backend processes
 */
bool
PoolEnqueuePMChild(uintptr_t pmchild_ptr)
{
	TimestampTz idle_since;
	
	if (pool_shmem == NULL || pool_shmem->queue == NULL)
		return false;
	
	if (pmchild_ptr == 0)
		return false;
	
	/* Get current timestamp when backend becomes idle */
	idle_since = GetCurrentTimestamp();
	
	/* Use the lock-free queue API */
	return LockFreeSlotQueueEnqueue(pool_shmem->queue,
									pmchild_ptr,
									idle_since,
									pool_shmem->max_pool_size);
}

/*
 * Add a backend to the idle pool (wrapper for compatibility)
 *
 * This is a wrapper that converts PMChild* to uintptr_t and calls
 * PoolEnqueuePMChild(). Can be called from postmaster or backend.
 */
void
PoolAddBackend(PMChild *pmchild)
{
	if (pmchild == NULL || pmchild->child_slot <= 0)
		return;
	
	if (PoolEnqueuePMChild((uintptr_t) pmchild))
	{
		elog(DEBUG2, "added backend (pid=%d, slot=%d) to connection pool",
			 (int) pmchild->pid, pmchild->child_slot);
	}
}

/*
 * Signal a backend process to exit gracefully
 *
 * This is a helper function to send SIGTERM to a backend process.
 * Called from postmaster context, so we can use kill() directly.
 */
static void
SignalBackendToExit(PMChild *pmchild)
{
	if (pmchild == NULL || pmchild->pid == 0)
		return;
	
	elog(DEBUG2, "sending SIGTERM to backend (pid=%d, slot=%d) to exit",
		 (int) pmchild->pid, pmchild->child_slot);
	
	if (kill(pmchild->pid, SIGTERM) < 0)
	{
		/* Process may have already exited */
		elog(DEBUG3, "kill(%d, SIGTERM) failed: %m", (int) pmchild->pid);
	}
	
#ifdef HAVE_SETSID
	/* Also signal the process group if supported */
	if (kill(-pmchild->pid, SIGTERM) < 0)
		elog(DEBUG3, "kill(%d, SIGTERM) failed: %m", (int) -pmchild->pid);
#endif
}

/*
 * Get an idle backend from the pool
 *
 * Returns NULL if no idle backend is available.
 * Simply dequeues from the queue - expired backends are handled by
 * PoolCleanupExpired() which is called periodically.
 *
 * THREAD SAFETY: Called only from postmaster's main thread (ServerLoop).
 */
PMChild *
PoolGetIdleBackend(void)
{
	if (pool_shmem == NULL || pool_shmem->queue == NULL)
		return NULL;
	
	/* Simply dequeue from the queue */
	return LockFreeSlotQueueDequeue(pool_shmem->queue, NULL);
}

PMChild *
PoolGetIdleBackendByDbName(const char *dbname, uint64 db_hash)
{
	LockFreeSlotQueue *queue;
	PMChild	   *pmchild = NULL;
	uint64		head;
	uint64		tail;
	uint32		scan_count;

	if (pool_shmem == NULL || pool_shmem->queue == NULL)
		return NULL;

	queue = pool_shmem->queue;

	if (dbname == NULL || dbname[0] == '\0' || db_hash == 0)
		return NULL;

	head = pg_atomic_read_u64(&queue->head);
	tail = pg_atomic_read_u64(&queue->tail);

	if (tail <= head)
		scan_count = 0;
	else
	{
		uint64 avail = tail - head;
		if (avail > queue->capacity)
			avail = queue->capacity;
		scan_count = (uint32) avail;
	}

	if (scan_count == 0)
		return NULL;

	for (uint32 i = 0; i < scan_count; i++)
	{
		TimestampTz idle_since;

		pmchild = LockFreeSlotQueueDequeue(queue, &idle_since);
		if (pmchild == NULL)
			break;

		if (pmchild->last_db_hash == db_hash && strcmp(pmchild->last_dbname, dbname) == 0)
			return pmchild;

		if (!LockFreeSlotQueueEnqueue(queue,
									  (uintptr_t) pmchild,
									  idle_since,
									  pool_shmem->max_pool_size))
			SignalBackendToExit(pmchild);
	}

	return NULL;
}

/*
 * Clean up expired backends from the pool (dynamic pool shrinking)
 *
 * This function is called periodically from the accept loop (ServerLoop).
 * It peeks at the first entry in the queue, checks if it's expired,
 * and accumulates timeout counts. When a backend has been expired
 * for multiple consecutive checks, it is dequeued, signaled to exit,
 * and removed from ActiveChildList.
 *
 * This enables dynamic pool shrinking: expired backends are gradually
 * removed from the pool, allowing the pool size to shrink naturally.
 */
void
PoolCleanupExpired(void)
{
	LockFreeSlotQueue *queue;
	uint64		head;
	uint64		tail;
	int			idle_count = 0;
	int			min_idle_size;
	TimestampTz now;
	TimestampTz idle_since;
	PMChild    *pmchild;
	const int	timeout_threshold = 3; /* Remove after 3 consecutive timeout checks */
	
	if (pool_shmem == NULL || pool_shmem->queue == NULL)
		return;

	queue = pool_shmem->queue;
	
	if (pool_shmem->idle_timeout <= 0)
		return; /* Timeout disabled */

	min_idle_size = pool_shmem->min_idle_size;
	if (min_idle_size < 0)
		min_idle_size = 0;
	if (min_idle_size > pool_shmem->max_pool_size)
		min_idle_size = pool_shmem->max_pool_size;

	head = pg_atomic_read_u64(&queue->head);
	tail = pg_atomic_read_u64(&queue->tail);
	if (tail > head)
	{
		uint64 avail = tail - head;
		if (avail > queue->capacity)
			avail = queue->capacity;
		idle_count = (int) avail;
	}

	if (idle_count <= min_idle_size)
	{
		timeout_count = 0;
		return;
	}
	
	now = GetCurrentTimestamp();
	
	/* Peek at the first entry in the queue (without dequeuing) */
	pmchild = LockFreeSlotQueuePeek(pool_shmem->queue, &idle_since);
	if (pmchild == NULL)
	{
		/* Queue is empty, reset timeout tracking */
		timeout_count = 0;
		return;
	}
	
	/* Check if this backend has been idle too long */
	if (idle_since > 0 &&
		TimestampDifferenceExceedsSeconds(idle_since, now, pool_shmem->idle_timeout))
	{
		/* Backend is expired, increment timeout counter */
		timeout_count++;
		
		elog(DEBUG2, "backend (pid=%d, slot=%d) expired check #%d (idle %ld seconds)",
			 (int) pmchild->pid, pmchild->child_slot, timeout_count,
			 (long) TimestampDifferenceMilliseconds(idle_since, now) / 1000);
		
		/* If threshold reached, remove this backend */
		if (timeout_count >= timeout_threshold)
		{
			/* Dequeue the entry (remove from queue) */
			PMChild *dequeued = LockFreeSlotQueueDequeue(pool_shmem->queue, NULL);
			
			/* Verify we got the same backend */
			if (dequeued == pmchild)
			{
				/* Send exit signal to the backend */
				SignalBackendToExit(pmchild);

				elog(LOG, "removed expired backend (pid=%d, slot=%d) from connection pool "
					 "(idle %ld seconds, %d consecutive timeout checks)",
					 (int) pmchild->pid, pmchild->child_slot,
					 (long) TimestampDifferenceMilliseconds(idle_since, now) / 1000,
					 timeout_count);
			}
			
			/* Reset timeout tracking */
			timeout_count = 0;
		}
	}
	else
	{
		/* Backend is not expired, reset timeout counter */
		timeout_count = 0;
	}
}

/*
 * Get current pool statistics
 */
void
PoolGetStats(int *current_size, int *max_size, int *idle_count)
{
	LockFreeSlotQueue *queue;
	uint64		head;
	uint64		tail;
	int			count = 0;
	
	if (pool_shmem == NULL)
	{
		*current_size = 0;
		*max_size = 0;
		*idle_count = 0;
		return;
	}
	
	*max_size = pool_shmem->max_pool_size;
	
	/* Count items in queue */
	if (pool_shmem->queue != NULL)
	{
		queue = pool_shmem->queue;
		head = pg_atomic_read_u64(&queue->head);
		tail = pg_atomic_read_u64(&queue->tail);
		if (tail > head)
		{
			uint64 avail = tail - head;
			if (avail > queue->capacity)
				avail = queue->capacity;
			count = (int) avail;
		}
	}
	
	*idle_count = count;
	*current_size = count; /* Approximate, actual size may vary */
}
