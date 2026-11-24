/* SPDX-License-Identifier: GPL-3.0 */

#include "passport.h"

#include <stddef.h>
#include <string.h>

#include "types.h"

static lb_passport_t g_lb_passport = {0};

void lb_passport_reset(void)
{
    (void)memset(&g_lb_passport, 0, sizeof(g_lb_passport));
}

lb_passport_t *lb_passport_mutable(void)
{
    return &g_lb_passport;
}

const lb_passport_t *lb_passport_get(void)
{
    return &g_lb_passport;
}
