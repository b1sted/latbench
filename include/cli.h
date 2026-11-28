/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_CLI_H
#define LB_CLI_H

#include <stdbool.h>

#include "bench.h"
#include "types.h"

/* Positional and mode metadata extracted alongside lb_cfg_t. */
typedef struct lb_cli_result_t {
    bool show_help;
    bool show_version;
    bool mode_set;
    bool mode_is_topo;
    const char *module_name;
    const char *subtest_name;
} lb_cli_result_t;

/* Populate cfg with the default runtime settings. */
void lb_cfg_init(lb_cfg_t *cfg);

const lb_module_t *lb_cli_find_module(const char *name);
const lb_subtest_t *lb_cli_find_subtest(const lb_module_t *module, const char *name);
bool lb_cli_mode_valid(const char *module, const char *subtest, bool mode_is_topo);
lb_status_t lb_cli_parse(int argc, char *const *argv, lb_cfg_t *cfg, lb_cli_result_t *out);

#endif /* LB_CLI_H */
