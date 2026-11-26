/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_PASSPORT_H
#define LB_PASSPORT_H

#include "types.h"

/*
 * Process-wide passport collector. Probes publish their headline results while
 * the "auto" profile runs, and the renderer reads them back once at the end.
 * The collector is reset before every auto run.
 */
void lb_passport_reset(void);

/* Mutable passport used by probes. Never returns NULL. */
lb_passport_t *lb_passport_mutable(void);

/* Read-only view used by the renderer and tests. Never returns NULL. */
const lb_passport_t *lb_passport_get(void);

#endif /* LB_PASSPORT_H */
