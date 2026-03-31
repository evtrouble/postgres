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
#include "utils/lsyscache.h"
#include "common/hashfn.h"
#include "miscadmin.h"

#define POOL_MAX_DATABASES 128

typedef struct DbPoolQueue
{
	pg_atomic_uint64 db_hash;
	char		dbname[NAMEDATALEN];
	LockFreeSlotQueue queue;
} DbPoolQueue;

/*
 * Connection pool shared memory structure
 */
typedef struct ConnectionPoolShmem
{
	DbPoolQueue *db_queues;
	int			max_pool_size;		/* Maximum pool size (from GUC) */
	int			current_pool_size;	/* Current number of backends in pool */
	int			idle_timeout;		/* Idle timeout in seconds (from GUC) */
	int			min_idle_size;		/* Minimum idle backends to keep */
	int			max_databases;
	uint32		per_db_queue_capacity;
} ConnectionPoolShmem;

/* Pointer to shared memory structure */
static ConnectionPoolShmem *pool_shmem = NULL;

/* GUC variables - accessed directly */
extern int connection_pool_size;
extern int connection_pool_idle_timeout;
extern int connection_pool_min_idle_size;
extern int connection_pool_db_num;

/*
 * Calculate shared memory size needed for connection pool
 *
 * NOTE: Queue capacity equals MaxBackends
 */
Size
ConnectionPoolShmemSize(void)
{
	Size		size;
	Size		qsize;
	uint32		per_db_capacity;
	uint32		max_dbs;
	int			max_pool;
	
	/* Size of ConnectionPoolShmem structure */
	size = sizeof(ConnectionPoolShmem);
	
	max_pool = connection_pool_size;
	if (max_pool <= 0)
		max_pool = 1;
	if (max_pool > 65536)
		max_pool = 65536;
	
	max_dbs = connection_pool_db_num;
	if (max_dbs <= 0)
		max_dbs = 1;
	if (max_dbs > POOL_MAX_DATABASES)
		max_dbs = POOL_MAX_DATABASES;
	
	per_db_capacity = (uint32) max_pool;
	
	qsize = sizeof(DbPoolQueue) * max_dbs;
	size = add_size(size, qsize);
	
	qsize = (Size) max_dbs * (Size) per_db_capacity * sizeof(PoolSlotEntry);
	size = add_size(size, qsize);
	return size;
}

/*
 * Initialize connection pool shared memory
 */
void
ConnectionPoolShmemInit(void)
{
	bool		found;
	bool		db_queues_found;
	bool		db_entries_found;
	Size		qsize;
	Size		esize;
	uint32		per_db_capacity;
	uint32		max_dbs;
	int			max_pool;
	
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
		pool_shmem->max_databases = POOL_MAX_DATABASES;
	}
	
	max_pool = connection_pool_size;
	if (max_pool <= 0)
		max_pool = 1;
	if (max_pool > 65536)
		max_pool = 65536;
	
	max_dbs = connection_pool_db_num;
	if (max_dbs <= 0)
		max_dbs = 1;
	if (max_dbs > POOL_MAX_DATABASES)
		max_dbs = POOL_MAX_DATABASES;
	
	per_db_capacity = (uint32) max_pool;
	pool_shmem->max_databases = max_dbs;
	
	qsize = sizeof(DbPoolQueue) * max_dbs;
	pool_shmem->db_queues = (DbPoolQueue *)
		ShmemInitStruct("Connection Pool DB Queues", qsize, &db_queues_found);
	
	{
		PoolSlotEntry *db_entries;
		
		esize = (Size) max_dbs * (Size) per_db_capacity * sizeof(PoolSlotEntry);
		db_entries = (PoolSlotEntry *)
			ShmemInitStruct("Connection Pool DB Queue Entries", esize, &db_entries_found);
		
		if (!db_queues_found && !db_entries_found)
		{
			for (uint32 i = 0; i < max_dbs; i++)
			{
				DbPoolQueue *dbq = &pool_shmem->db_queues[i];
				PoolSlotEntry *entries = db_entries + (Size) i * per_db_capacity;
				
				pg_atomic_init_u64(&dbq->db_hash, 0);
				dbq->dbname[0] = '\0';
				if (!LockFreeSlotQueueInit(&dbq->queue,
										   per_db_capacity,
										   entries))
					elog(ERROR, "failed to initialize per-database connection pool queue");
			}
		}
		else
		{
			if (pool_shmem->per_db_queue_capacity != per_db_capacity)
				elog(WARNING, "connection pool per-db queue capacity mismatch: expected %u, got %u",
					 per_db_capacity, pool_shmem->per_db_queue_capacity);
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
	DbPoolQueue *db_queues;
	uint32		max_dbs;
	uint64		db_hash;
	char		dbname_trunc[NAMEDATALEN];
	const char *dbname_src;
	Size		dbname_len;
	int			free_index = -1;
	
	if (pool_shmem == NULL || pool_shmem->db_queues == NULL)
		return false;
	
	if (pmchild_ptr == 0)
		return false;
	
	if (MyProcPort == NULL || MyProcPort->database_name == NULL)
		return false;
	
	dbname_trunc[0] = '\0';
	dbname_src = MyProcPort->database_name;
	strlcpy(dbname_trunc, dbname_src, sizeof(dbname_trunc));
	dbname_len = strlen(dbname_trunc);
	if (dbname_len == 0)
		return false;
	db_hash = hash_bytes_extended((const unsigned char *) dbname_trunc,
								  (int) dbname_len,
								  0);
	if (db_hash == 0)
		return false;
	
	idle_since = GetCurrentTimestamp();
	
	db_queues = pool_shmem->db_queues;
	max_dbs = (uint32) pool_shmem->max_databases;
	
	for (uint32 i = 0; i < max_dbs; i++)
	{
		DbPoolQueue *dbq = &db_queues[i];
		uint64		cur_hash;
		
		cur_hash = pg_atomic_read_u64(&dbq->db_hash);
		if (cur_hash == 0)
		{
			if (free_index < 0)
				free_index = (int) i;
			continue;
		}
		
		if (cur_hash == db_hash &&
			strcmp(dbq->dbname, dbname_trunc) == 0)
		{
			return LockFreeSlotQueueEnqueue(&dbq->queue,
											pmchild_ptr,
											idle_since,
											pool_shmem->max_pool_size);
		}
	}
	
	if (free_index >= 0)
	{
		DbPoolQueue *dbq = &db_queues[free_index];
		uint64		expected = 0;
		
		if (pg_atomic_compare_exchange_u64(&dbq->db_hash, &expected, db_hash) ||
			(expected == db_hash &&
			 strcmp(dbq->dbname, dbname_trunc) == 0))
		{
			if (expected == 0)
				strlcpy(dbq->dbname, dbname_trunc, sizeof(dbq->dbname));
			
			return LockFreeSlotQueueEnqueue(&dbq->queue,
											pmchild_ptr,
											idle_since,
											pool_shmem->max_pool_size);
		}
		
		for (uint32 i = 0; i < max_dbs; i++)
		{
			DbPoolQueue *dbq2 = &db_queues[i];
			uint64		cur_hash;
			
			cur_hash = pg_atomic_read_u64(&dbq2->db_hash);
			if (cur_hash == db_hash &&
				strcmp(dbq2->dbname, dbname_trunc) == 0)
			{
				return LockFreeSlotQueueEnqueue(&dbq2->queue,
												pmchild_ptr,
												idle_since,
												pool_shmem->max_pool_size);
			}
		}
	}
	
	return false;
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
	if (pool_shmem == NULL || pool_shmem->db_queues == NULL)
		return NULL;
	
	for (int i = 0; i < pool_shmem->max_databases; i++)
	{
		DbPoolQueue *dbq = &pool_shmem->db_queues[i];
		PMChild    *pmchild;
		uint64		cur_hash;
		
		cur_hash = pg_atomic_read_u64(&dbq->db_hash);
		if (cur_hash == 0)
			continue;
		
		pmchild = LockFreeSlotQueueDequeue(&dbq->queue, NULL);
		if (pmchild != NULL)
			return pmchild;
	}
	
	return NULL;
}

PMChild *
PoolGetIdleBackendByDbName(const char *dbname, uint64 db_hash)
{
	DbPoolQueue *db_queues;
	int			free_index = -1;

	if (pool_shmem == NULL || pool_shmem->db_queues == NULL)
		return NULL;

	db_queues = pool_shmem->db_queues;

	if (dbname == NULL || dbname[0] == '\0' || db_hash == 0)
		return NULL;

	for (int i = 0; i < pool_shmem->max_databases; i++)
	{
		DbPoolQueue *dbq = &db_queues[i];
		uint64		cur_hash;
		
		cur_hash = pg_atomic_read_u64(&dbq->db_hash);
		if (cur_hash == db_hash &&
			strcmp(dbq->dbname, dbname) == 0)
		{
			return LockFreeSlotQueueDequeue(&dbq->queue, NULL);
		}
		if (cur_hash == 0 && free_index < 0)
			free_index = i;
	}

	if (free_index >= 0)
	{
		DbPoolQueue *dbq = &db_queues[free_index];
		uint64		expected = 0;
		
		if (pg_atomic_compare_exchange_u64(&dbq->db_hash, &expected, db_hash) ||
			(expected == db_hash &&
			 strcmp(dbq->dbname, dbname) == 0))
		{
			if (expected == 0)
				strlcpy(dbq->dbname, dbname, sizeof(dbq->dbname));
		}
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
	uint64		head;
	uint64		tail;
	int			idle_count = 0;
	int			min_idle_size;
	TimestampTz now;
	TimestampTz idle_since;
	PMChild    *pmchild;
	LockFreeSlotQueue *queue = NULL;
	
	if (pool_shmem == NULL || pool_shmem->db_queues == NULL)
		return;
	
	if (pool_shmem->idle_timeout <= 0)
		return; /* Timeout disabled */

	min_idle_size = pool_shmem->min_idle_size;
	if (min_idle_size < 0)
		min_idle_size = 0;
	if (min_idle_size > pool_shmem->max_pool_size)
		min_idle_size = pool_shmem->max_pool_size;

	for (int i = 0; i < pool_shmem->max_databases; i++)
	{
		DbPoolQueue *dbq = &pool_shmem->db_queues[i];
		LockFreeSlotQueue *q = &dbq->queue;
		uint64		cur_hash;
		
		cur_hash = pg_atomic_read_u64(&dbq->db_hash);
		if (cur_hash == 0)
			continue;
		
		head = pg_atomic_read_u64(&q->head);
		tail = pg_atomic_read_u64(&q->tail);
		if (tail > head)
		{
			uint64 avail = tail - head;
			if (avail > q->capacity)
				avail = q->capacity;
			idle_count += (int) avail;
		}
	}

	if (idle_count <= min_idle_size)
	{
		return;
	}
	
	now = GetCurrentTimestamp();
	
	for (int i = 0; i < pool_shmem->max_databases; i++)
	{
		DbPoolQueue *dbq = &pool_shmem->db_queues[i];
		uint64		cur_hash;
		
		cur_hash = pg_atomic_read_u64(&dbq->db_hash);
		if (cur_hash == 0)
			continue;
		
		pmchild = LockFreeSlotQueuePeek(&dbq->queue, &idle_since);
		if (pmchild != NULL &&
			idle_since > 0 &&
			TimestampDifferenceExceedsSeconds(idle_since, now, pool_shmem->idle_timeout))
		{
			queue = &dbq->queue;
			break;
		}
	}
	if (pmchild == NULL || queue == NULL)
	{
		return;
	}
	
	pmchild = LockFreeSlotQueueDequeue(queue, NULL);
	if (pmchild == NULL)
		return;

	SignalBackendToExit(pmchild);

	elog(LOG, "removed expired backend (pid=%d, slot=%d) from connection pool "
			 "(idle %ld seconds)",
			 (int) pmchild->pid, pmchild->child_slot,
			 (long) TimestampDifferenceMilliseconds(idle_since, now) / 1000);
}

/*
 * Get current pool statistics
 */
void
PoolGetStats(int *current_size, int *max_size, int *idle_count)
{
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
	
	if (pool_shmem->db_queues != NULL)
	{
		for (int i = 0; i < pool_shmem->max_databases; i++)
		{
			DbPoolQueue *dbq = &pool_shmem->db_queues[i];
			LockFreeSlotQueue *queue = &dbq->queue;
			uint64		cur_hash;
			
			cur_hash = pg_atomic_read_u64(&dbq->db_hash);
			if (cur_hash == 0)
				continue;
			
			head = pg_atomic_read_u64(&queue->head);
			tail = pg_atomic_read_u64(&queue->tail);
			if (tail > head)
			{
				uint64 avail = tail - head;
				if (avail > queue->capacity)
					avail = queue->capacity;
				count += (int) avail;
			}
		}
	}
	
	*idle_count = count;
	*current_size = count; /* Approximate, actual size may vary */
}
