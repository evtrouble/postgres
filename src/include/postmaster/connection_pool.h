/*-------------------------------------------------------------------------
 *
 * connection_pool.h
 *	  Header for lock-free connection pool implementation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/postmaster/connection_pool.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include "postmaster/postmaster.h"
#include "libpq/libpq-be.h"

/* Shared memory functions */
extern Size ConnectionPoolShmemSize(void);
extern void ConnectionPoolShmemInit(void);

/* Connection pool functions */
extern void InitConnectionPool(void);
extern void ConfigureConnectionPool(void);
extern void PoolAddBackend(PMChild *pmchild);
extern PMChild *PoolGetIdleBackend(void);
extern void PoolCleanupExpired(void);
extern void PoolGetStats(int *current_size, int *max_size, int *idle_count);

/* Lock-free queue functions (can be called from backend processes) */
extern bool PoolEnqueuePMChild(uintptr_t pmchild_ptr);
#endif							/* CONNECTION_POOL_H */

