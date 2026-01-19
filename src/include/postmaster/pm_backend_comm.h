/*-------------------------------------------------------------------------
 *
 * postmaster.h
 *	  Exports from postmaster/pm_backend_comm.c.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/pm_backend_comm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PM_BACKEND_COMM_H
#define _PM_BACKEND_COMM_H

#include "libpq/libpq-be.h"

extern bool send_socket_to_backend(PMChild *pmchild, ClientSocket *client_sock);

extern void receive_socket_from_postmaster(ClientSocket *cs);

extern void init_pool_backend_socket(void);
#endif