/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#ifndef _FR_SCHEDULE_H
#define _FR_SCHEDULE_H
/**
 * $Id$
 *
 * @file util/schedule.h
 * @brief Scheduler communication.
 *
 * @copyright 2016 Alan DeKok <aland@freeradius.org>
 */
RCSIDH(schedule_h, "$Id$")

#include <freeradius-devel/util/worker.h>
#include <freeradius-devel/fr_log.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fr_schedule_t fr_schedule_t;
typedef int (*fr_schedule_thread_instantiate_t)(void *ctx);

fr_schedule_t *fr_schedule_create(TALLOC_CTX *ctx, fr_log_t *log, int max_inputs, int max_workers,
				  uint32_t num_transports, fr_transport_t **transports,
				  fr_schedule_thread_instantiate_t worker_thread_instantiate,
				  void *worker_thread_ctx);
/* schedulers are async, so there's no fr_schedule_run() */
int fr_schedule_destroy(fr_schedule_t *sc);
int fr_schedule_get_worker_kq(fr_schedule_t *sc);

int fr_schedule_socket_add(fr_schedule_t *sc, int fd, void *ctx, fr_transport_t *transport) CC_HINT(nonnull);

#ifdef __cplusplus
}
#endif

#endif /* _FR_SCHEDULE_H */
