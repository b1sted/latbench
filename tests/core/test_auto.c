/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stdio.h>

#include "auto.h"
#include "passport.h"
#include "types.h"

int main(void)
{
    lb_cfg_t cfg = {0};
    lb_status_t status = LB_STATUS_ERROR;
    const lb_passport_t *passport = NULL;
    int failures = 0;

    cfg.cpu_id = -1;
    cfg.batch_ms = 1u;
    cfg.max_mem_mb = 4u;
    cfg.no_thp = true;
    cfg.quiet = true;
    cfg.os_ctx_mode = LB_OS_CTX_BOTH;

    status = lb_auto_run(&cfg);
    if (status != LB_STATUS_OK) {
        fprintf(stderr, "FAIL: lb_auto_run returned status %d\n", (int)status);
        failures++;
    }

    passport = lb_passport_get();
    if (passport->arch == NULL) {
        fprintf(stderr, "FAIL: auto did not publish the architecture\n");
        failures++;
    }
    if (passport->page_size == 0u) {
        fprintf(stderr, "FAIL: auto did not publish the page size\n");
        failures++;
    }
    if (passport->cpu_count == 0u) {
        fprintf(stderr, "FAIL: auto did not publish the CPU count\n");
        failures++;
    }

    printf("test_auto: %s\n", (failures == 0) ? "ok" : "FAILED");
    return (failures == 0) ? 0 : 1;
}
