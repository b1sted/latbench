/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "defs.h"
#include "pal/pal.h"
#include "tty.h"
#include "types.h"
#include "util.h"

#define LB_VERSION "0.1.0"
#define LB_DEFAULT_BATCH_MS 20u
#define LB_DEFAULT_COOL_MS 0u

#define HELP_FLAG_FMT "  %-22s %s"

static const lb_subtest_t SYS_SUBTESTS[] = {
    {"info", "platform limits and runtime settings", sys_info_run},
    {"timer", "clock resolution and pal_now_ns() overhead", sys_timer_run},
};

static const lb_module_t BENCH_MODULES[] = {
    {"sys", "system calibration and clock diagnostics", SYS_SUBTESTS, LB_ARRAY_SIZE(SYS_SUBTESTS)},
};

const lb_module_t *lb_bench_modules(size_t *count)
{
    if (count != NULL) {
        *count = LB_ARRAY_SIZE(BENCH_MODULES);
    }
    return BENCH_MODULES;
}

static const lb_module_t *find_module(const char *name)
{
    const lb_module_t *modules = NULL;
    size_t count = 0u;
    size_t i = 0u;

    modules = lb_bench_modules(&count);
    if (name == NULL) {
        return NULL;
    }

    for (i = 0u; i < count; i++) {
        if (strcmp(modules[i].name, name) == 0) {
            return &modules[i];
        }
    }
    return NULL;
}

static const lb_subtest_t *find_subtest(const lb_module_t *module, const char *name)
{
    size_t i = 0u;

    if (module == NULL || name == NULL) {
        return NULL;
    }

    for (i = 0u; i < module->subtest_count; i++) {
        if (strcmp(module->subtests[i].name, name) == 0) {
            return &module->subtests[i];
        }
    }
    return NULL;
}

static void print_help(const lb_cfg_t *cfg)
{
    const lb_module_t *modules = NULL;
    size_t module_count = 0u;
    size_t i = 0u;

    modules = lb_bench_modules(&module_count);

    tty_header(cfg, "Usage: latbench <module> <subtest> [OPTIONS]");
    tty_header(cfg, "%s", "");

    tty_section(cfg, "Runtime");
    tty_header(cfg, HELP_FLAG_FMT, "    --cpu <id>", "pin worker to logical CPU <id>");
    tty_header(cfg, HELP_FLAG_FMT, "    --qos <perf|bg>", "request perf or bg QoS");
    tty_header(cfg, HELP_FLAG_FMT, "    --max-mem <MiB>", "cap arena size (default: 25% of RAM)");
    tty_header(cfg, HELP_FLAG_FMT, "    --batch-ms <ms>",
               "measurement batch duration (default: 20)");
    tty_header(cfg, HELP_FLAG_FMT, "    --cool-ms <ms>", "cool-down between batches (default: 0)");
    tty_header(cfg, HELP_FLAG_FMT, "    --no-thp", "avoid transparent huge pages (default)");
    tty_header(cfg, HELP_FLAG_FMT, "    --thp", "allow transparent huge pages");

    tty_section(cfg, "Output");
    tty_header(cfg, HELP_FLAG_FMT, "    --csv", "emit CSV to stdout; diagnostics to stderr");
    tty_header(cfg, HELP_FLAG_FMT, "    --no-color", "disable color output");

    tty_section(cfg, "System");
    tty_header(cfg, HELP_FLAG_FMT, "-h, --help", "display this help and exit");
    tty_header(cfg, HELP_FLAG_FMT, "-V, --version", "output version information and exit");

    tty_section(cfg, "Modules");
    for (i = 0u; i < module_count; i++) {
        size_t j = 0u;

        for (j = 0u; j < modules[i].subtest_count; j++) {
            char label[LB_NAME_MAX] = {0};

            (void)snprintf(label, sizeof(label), "%s %s", modules[i].name,
                           modules[i].subtests[j].name);
            tty_header(cfg, HELP_FLAG_FMT, label, modules[i].subtests[j].help);
        }
    }
}

static void print_help_to_stderr(const lb_cfg_t *cfg)
{
    lb_cfg_t err_cfg = *cfg;

    err_cfg.csv = true;
    print_help(&err_cfg);
}

static lb_status_t parse_u32_option(const char *option, const char *text, uint32_t *out)
{
    uint64_t value = 0u;

    if (util_parse_u64(text, &value) != LB_STATUS_OK || value > (uint64_t)UINT32_MAX) {
        tty_err("invalid value for %s: '%s'", option, text);
        return LB_STATUS_INVALID_ARG;
    }

    *out = (uint32_t)value;
    return LB_STATUS_OK;
}

static lb_status_t parse_cpu_option(const char *text, int32_t *out)
{
    int32_t value = 0;
    const lb_status_t status = util_parse_i32(text, &value);

    if (status != LB_STATUS_OK || value < 0) {
        tty_err("invalid value for --cpu: '%s'", text);
        return LB_STATUS_INVALID_ARG;
    }

    *out = value;
    return LB_STATUS_OK;
}

static lb_status_t parse_qos_option(const char *text, lb_cfg_t *cfg)
{
    if (strcmp(text, "perf") == 0) {
        cfg->qos_perf = true;
    } else if (strcmp(text, "bg") == 0) {
        cfg->qos_perf = false;
    } else {
        tty_err("invalid value for --qos: '%s' (expected perf or bg)", text);
        return LB_STATUS_INVALID_ARG;
    }

    cfg->qos_set = true;
    return LB_STATUS_OK;
}

static lb_status_t parse_args(int argc, char **argv, lb_cfg_t *cfg, bool *show_help,
                              bool *show_version, const char **module_name,
                              const char **subtest_name)
{
    const char *positionals[2] = {NULL, NULL};
    int positional_count = 0;
    int i = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            *show_help = true;
            return LB_STATUS_OK;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            *show_version = true;
            return LB_STATUS_OK;
        }
        if (strcmp(arg, "--no-color") == 0) {
            cfg->no_color = true;
            tty_set_no_color(true);
            continue;
        }
        if (strcmp(arg, "--csv") == 0) {
            cfg->csv = true;
            continue;
        }
        if (strcmp(arg, "--no-thp") == 0) {
            cfg->no_thp = true;
            continue;
        }
        if (strcmp(arg, "--thp") == 0) {
            cfg->no_thp = false;
            continue;
        }

        if (strcmp(arg, "--cpu") == 0 || strcmp(arg, "--qos") == 0 ||
            strcmp(arg, "--max-mem") == 0 || strcmp(arg, "--batch-ms") == 0 ||
            strcmp(arg, "--cool-ms") == 0) {
            const char *value = NULL;

            if (i + 1 >= argc) {
                tty_err("option %s requires a value", arg);
                return LB_STATUS_INVALID_ARG;
            }
            i++;
            value = argv[i];

            if (strcmp(arg, "--cpu") == 0) {
                if (parse_cpu_option(value, &cfg->cpu_id) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            } else if (strcmp(arg, "--qos") == 0) {
                if (parse_qos_option(value, cfg) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            } else if (strcmp(arg, "--max-mem") == 0) {
                if (parse_u32_option(arg, value, &cfg->max_mem_mb) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            } else if (strcmp(arg, "--batch-ms") == 0) {
                if (parse_u32_option(arg, value, &cfg->batch_ms) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            } else {
                if (parse_u32_option(arg, value, &cfg->cool_ms) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            }
            continue;
        }

        if (arg[0] == '-') {
            tty_err("unknown option '%s'", arg);
            return LB_STATUS_INVALID_ARG;
        }
        if (positional_count >= 2) {
            tty_err("unexpected argument '%s'", arg);
            return LB_STATUS_INVALID_ARG;
        }

        positionals[positional_count] = arg;
        positional_count++;
    }

    *module_name = positionals[0];
    *subtest_name = positionals[1];
    return LB_STATUS_OK;
}

int main(int argc, char **argv)
{
    lb_cfg_t cfg = {0};
    bool show_help = false;
    bool show_version = false;
    const char *module_name = NULL;
    const char *subtest_name = NULL;
    const lb_module_t *module = NULL;
    const lb_subtest_t *subtest = NULL;
    lb_status_t status = LB_STATUS_OK;

    cfg.cpu_id = -1;
    cfg.qos_perf = false;
    cfg.qos_set = false;
    cfg.max_mem_mb = 0u;
    cfg.batch_ms = LB_DEFAULT_BATCH_MS;
    cfg.cool_ms = LB_DEFAULT_COOL_MS;
    cfg.no_thp = true;
    cfg.csv = false;
    cfg.no_color = false;

    status = parse_args(argc, argv, &cfg, &show_help, &show_version, &module_name, &subtest_name);
    if (status != LB_STATUS_OK) {
        print_help_to_stderr(&cfg);
        return 2;
    }

    tty_set_no_color(cfg.no_color);

    if (show_version) {
        tty_header(NULL, "latbench v%s (%s)", LB_VERSION, pal_arch_name());
        return 0;
    }

    if (show_help) {
        print_help(&cfg);
        return 0;
    }

    if (module_name == NULL) {
        if (argc == 1) {
            print_help(&cfg);
            return 0;
        }
        tty_err("missing module name");
        print_help_to_stderr(&cfg);
        return 2;
    }

    if (subtest_name == NULL) {
        tty_err("missing subtest for module '%s'", module_name);
        print_help_to_stderr(&cfg);
        return 2;
    }

    module = find_module(module_name);
    if (module == NULL) {
        tty_err("unknown module '%s'", module_name);
        print_help_to_stderr(&cfg);
        return 2;
    }

    subtest = find_subtest(module, subtest_name);
    if (subtest == NULL) {
        tty_err("unknown subtest '%s' for module '%s'", subtest_name, module_name);
        print_help_to_stderr(&cfg);
        return 2;
    }

    if (subtest->run == NULL) {
        tty_err("subtest '%s %s' is not implemented", module_name, subtest_name);
        return 1;
    }

    status = subtest->run(&cfg);
    if (status != LB_STATUS_OK) {
        tty_err("subtest '%s %s' failed (status %d)", module_name, subtest_name, (int)status);
        return 1;
    }

    return 0;
}
