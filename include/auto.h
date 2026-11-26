/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_AUTO_H
#define LB_AUTO_H

#include "types.h"

/*
 * Run the compact full-machine profile: every key probe with one shared
 * configuration, followed by the Passport card (TTY) or passport CSV rows.
 * The caller's configuration is not modified.
 */
lb_status_t lb_auto_run(const lb_cfg_t *cfg);

#endif /* LB_AUTO_H */
