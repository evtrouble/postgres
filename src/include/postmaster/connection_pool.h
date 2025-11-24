/*-------------------------------------------------------------------------
 *
 * connection_pool.h
 *	  Header for connection pool implementation
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

/* Connection pool functions */
extern void InitConnectionPool(void);
extern void ConfigureConnectionPool(int max_size, int idle_timeout);
extern void PoolAddBackend(PMChild *pmchild);
extern PMChild *PoolGetIdleBackend(void);
extern void PoolRemoveBackend(PMChild *pmchild);
extern void PoolCleanupExpired(void);
extern void PoolGetStats(int *current_size, int *max_size, int *idle_count);

#endif							/* CONNECTION_POOL_H */

