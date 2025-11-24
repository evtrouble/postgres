/*-------------------------------------------------------------------------
 *
 * connection_pool.c
 *	  Basic connection pool implementation for PostgreSQL
 *
 * This file implements a simple connection pool that manages backend
 * processes for reuse. The pool maintains a list of idle backends that
 * can be reused for new client connections.
 *
 * DESIGN NOTES:
 * =============
 *
 * 1. Pool Structure:
 *    - We maintain a list of idle backends (backends not currently
 *      serving a client)
 *    - Each backend can be in one of three states:
 *      * IDLE: Available for reuse
 *      * BUSY: Currently serving a client
 *      * EXPIRED: Idle for too long, should be terminated
 *
 * 2. Key Operations:
 *    - acquire_backend(): Get an idle backend from the pool, or create
 *      a new one if pool is empty
 *    - release_backend(): Return a backend to the pool when it finishes
 *      serving a client
 *    - cleanup_expired(): Remove backends that have been idle too long
 *
 * 3. Thread Safety and Concurrency:
 *    
 *    WHY NO LOCKS ARE NEEDED:
 *    ========================
 *    - PostgreSQL postmaster is SINGLE-THREADED by design
 *    - All connection pool operations happen in ServerLoop(), which is a
 *      single-threaded event loop
 *    - Operations are serialized: one connection at a time is processed
 *    - Backend processes are separate processes (not threads), they don't
 *      directly access the connection pool data structures
 *    
 *    CONCURRENCY MODEL:
 *    ==================
 *    - Postmaster thread: All pool operations (add/get/remove) happen here
 *    - Backend processes: Independent processes, communicate via signals/
 *      shared memory, don't touch pool structures directly
 *    - Signal handlers: Use volatile flags, actual work done in ServerLoop
 *    
 *    WHEN LOCKS MIGHT BE NEEDED (FUTURE):
 *    =====================================
 *    - If we add async I/O operations
 *    - If we add background threads for pool maintenance
 *    - If we allow direct pool access from backend processes
 *    - In these cases, we'd need LWLock or similar synchronization
 *
 * 4. Integration Points:
 *    - Will be called from ServerLoop() when a new connection arrives
 *    - Will interact with PMChild structures to track backend state
 *    - Will use postmaster_child_launch() to create new backends
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

#include "postmaster/connection_pool.h"
#include "postmaster/postmaster.h"
#include "utils/timestamp.h"

/*
 * Connection pool state
 *
 * NOTE ON THREAD SAFETY:
 * ======================
 * These variables are accessed ONLY from the postmaster's main thread
 * (ServerLoop). PostgreSQL postmaster is single-threaded by design:
 *
 * 1. All operations are serialized in ServerLoop() event loop
 * 2. Backend processes are separate processes (forked), not threads
 * 3. Signal handlers only set volatile flags; actual work is done in ServerLoop
 * 4. No concurrent access = no locks needed
 *
 * If we ever need to support:
 * - Async I/O operations
 * - Background maintenance threads
 * - Direct pool access from backend processes
 * Then we would need to add LWLock or similar synchronization.
 */
static dlist_head	IdleBackendList;	/* List of idle backends */
static int			PoolSize = 0;		/* Current number of backends in pool */
static int			MaxPoolSize = 10;	/* Maximum pool size (from GUC) */
static int			IdleTimeout = 300;	/* Idle timeout in seconds (from GUC) */

/*
 * Backend state in the connection pool
 */
typedef enum
{
	POOL_BACKEND_IDLE,		/* Available for reuse */
	POOL_BACKEND_BUSY,		/* Currently serving a client */
	POOL_BACKEND_EXPIRED	/* Idle too long, should be terminated */
} PoolBackendState;

/*
 * Extended backend information for connection pool
 */
typedef struct PoolBackend
{
	PMChild	   *pmchild;			/* Pointer to PMChild structure */
	PoolBackendState state;			/* Current state */
	TimestampTz idle_since;			/* When this backend became idle */
	dlist_node	elem;				/* List link in IdleBackendList */
} PoolBackend;

/*
 * Initialize the connection pool
 */
void
InitConnectionPool(void)
{
	dlist_init(&IdleBackendList);
	PoolSize = 0;
	
	elog(DEBUG1, "connection pool initialized (max_size=%d, idle_timeout=%d)",
		 MaxPoolSize, IdleTimeout);
}

/*
 * Set pool configuration from GUC variables
 */
void
ConfigureConnectionPool(int max_size, int idle_timeout)
{
	MaxPoolSize = max_size;
	IdleTimeout = idle_timeout;
	
	elog(DEBUG1, "connection pool configured (max_size=%d, idle_timeout=%d)",
		 MaxPoolSize, IdleTimeout);
}

/*
 * Add a backend to the idle pool
 *
 * This is called when a backend finishes serving a client and becomes
 * available for reuse.
 *
 * THREAD SAFETY: Called only from postmaster's main thread (ServerLoop),
 * no locking needed.
 */
void
PoolAddBackend(PMChild *pmchild)
{
	PoolBackend *pool_backend;
	
	/* Check if pool is full */
	if (PoolSize >= MaxPoolSize)
	{
		elog(DEBUG2, "connection pool is full (%d/%d), not adding backend",
			 PoolSize, MaxPoolSize);
		return;
	}
	
	/* Allocate pool backend structure */
	pool_backend = (PoolBackend *) palloc(sizeof(PoolBackend));
	pool_backend->pmchild = pmchild;
	pool_backend->state = POOL_BACKEND_IDLE;
	pool_backend->idle_since = GetCurrentTimestamp();
	
	/* Add to idle list */
	dlist_push_head(&IdleBackendList, &pool_backend->elem);
	PoolSize++;
	
	elog(DEBUG2, "added backend (pid=%d) to connection pool (size=%d/%d)",
		 (int) pmchild->pid, PoolSize, MaxPoolSize);
}

/*
 * Get an idle backend from the pool
 *
 * Returns NULL if no idle backend is available.
 *
 * THREAD SAFETY: Called only from postmaster's main thread (ServerLoop),
 * no locking needed.
 */
PMChild *
PoolGetIdleBackend(void)
{
	PoolBackend *pool_backend;
	PMChild	   *pmchild;
	
	/* Check if pool is empty */
	if (dlist_is_empty(&IdleBackendList))
	{
		elog(DEBUG2, "connection pool is empty");
		return NULL;
	}
	
	/* Get the first idle backend */
	pool_backend = dlist_container(PoolBackend, elem,
								   dlist_pop_head_node(&IdleBackendList));
	pmchild = pool_backend->pmchild;
	pool_backend->state = POOL_BACKEND_BUSY;
	PoolSize--;
	
	elog(DEBUG2, "acquired backend (pid=%d) from connection pool (size=%d/%d)",
		 (int) pmchild->pid, PoolSize, MaxPoolSize);
	
	/* Free the pool backend structure */
	pfree(pool_backend);
	
	return pmchild;
}

/*
 * Remove a backend from the pool
 *
 * This is called when a backend exits or needs to be removed.
 */
void
PoolRemoveBackend(PMChild *pmchild)
{
	dlist_iter	iter;
	PoolBackend *pool_backend;
	bool		found = false;
	
	/* Search for the backend in the idle list */
	dlist_foreach(iter, &IdleBackendList)
	{
		pool_backend = dlist_container(PoolBackend, elem, iter.cur);
		if (pool_backend->pmchild == pmchild)
		{
			dlist_delete(&pool_backend->elem);
			PoolSize--;
			found = true;
			pfree(pool_backend);
			break;
		}
	}
	
	if (found)
	{
		elog(DEBUG2, "removed backend (pid=%d) from connection pool (size=%d/%d)",
			 (int) pmchild->pid, PoolSize, MaxPoolSize);
	}
}

/*
 * Clean up expired backends from the pool
 *
 * This should be called periodically to remove backends that have been
 * idle for too long.
 */
void
PoolCleanupExpired(void)
{
	dlist_mutable_iter iter;
	PoolBackend *pool_backend;
	TimestampTz now;
	int			expired_count = 0;
	
	if (dlist_is_empty(&IdleBackendList))
		return;
	
	now = GetCurrentTimestamp();
	
	/* Iterate through idle backends and remove expired ones */
	dlist_foreach_modify(iter, &IdleBackendList)
	{
		pool_backend = dlist_container(PoolBackend, elem, iter.cur);
		
		/* Check if backend has been idle too long */
		if (TimestampDifferenceExceedsSeconds(pool_backend->idle_since, now,
											   IdleTimeout))
		{
			elog(DEBUG2, "removing expired backend (pid=%d) from pool",
				 (int) pool_backend->pmchild->pid);
			
			dlist_delete(iter.cur);
			PoolSize--;
			expired_count++;
			
			/* TODO: Actually terminate the backend process */
			/* For now, just remove from pool */
			
			pfree(pool_backend);
		}
	}
	
	if (expired_count > 0)
	{
		elog(DEBUG1, "cleaned up %d expired backend(s) from connection pool",
			 expired_count);
	}
}

/*
 * Get current pool statistics
 */
void
PoolGetStats(int *current_size, int *max_size, int *idle_count)
{
	*current_size = PoolSize;
	*max_size = MaxPoolSize;
	*idle_count = PoolSize; /* All in pool are idle */
}

