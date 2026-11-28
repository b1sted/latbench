/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "tty.h"
#include "types.h"

#define TEST_CAPTURE_ERR "test_cli_capture.err"
#define TEST_MAX_ARGS 32
#define TEST_ARG_LEN 64u

static int g_checks = 0;
static int g_failures = 0;

static void check_true(bool condition, const char *name)
{
    g_checks++;
    if (!condition) {
        fprintf(stdout, "FAIL: %s\n", name);
        g_failures++;
    }
}

static lb_status_t parse_list(const char *const *args, int count, lb_cfg_t *cfg,
                              lb_cli_result_t *res)
{
    /*
     * lb_cli_parse() keeps pointers into argv, so the backing storage must
     * outlive the call. Keep it static; the tests only inspect one parse
     * result before issuing the next call.
     */
    static char storage[TEST_MAX_ARGS][TEST_ARG_LEN];
    static char *argv[TEST_MAX_ARGS];
    int i = 0;

    if (count < 0 || count > TEST_MAX_ARGS) {
        return LB_STATUS_INVALID_ARG;
    }
    for (i = 0; i < count; i++) {
        (void)snprintf(storage[i], sizeof(storage[i]), "%s", args[i]);
        argv[i] = storage[i];
    }

    lb_cfg_init(cfg);
    return lb_cli_parse(count, argv, cfg, res);
}

static void test_defaults(void)
{
    lb_cfg_t cfg = {0};
    lb_cli_result_t res = {0};
    const char *const args[1] = {"latbench"};
    const lb_status_t status = parse_list(args, 1, &cfg, &res);

    check_true(status == LB_STATUS_OK, "bare invocation parses cleanly");
    check_true(cfg.cpu_id == -1, "default cpu_id is -1");
    check_true(cfg.batch_ms == 20u, "default batch_ms is 20");
    check_true(cfg.cool_ms == 0u, "default cool_ms is 0");
    check_true(cfg.no_thp && !cfg.thp_set, "default THP policy is no-thp and unset");
    check_true(cfg.os_ctx_mode == LB_OS_CTX_BOTH, "default os ctx mode is both");
    check_true(res.module_name == NULL && res.subtest_name == NULL, "no positional args");
}

static void test_valid_flags(void)
{
    lb_cfg_t cfg = {0};
    lb_cli_result_t res = {0};
    const char *const dram_args[] = {
        "latbench", "dram",      "bw",        "--cpu",      "2",     "--qos",
        "perf",     "--max-mem", "128",       "--batch-ms", "50",    "--cool-ms",
        "10",       "--thp",     "--threads", "4",          "--csv", "--no-color",
    };
    const char *const ctx_args[] = {"latbench", "os", "ctx", "--mode", "proc"};
    const char *const help_args[] = {"latbench", "--help", "--no-color"};
    const char *const ver_args[] = {"latbench", "-V"};

    check_true(parse_list(dram_args, (int)(sizeof(dram_args) / sizeof(dram_args[0])), &cfg, &res) ==
                   LB_STATUS_OK,
               "dram bw with full options succeeds");
    check_true(strcmp(res.module_name, "dram") == 0 && strcmp(res.subtest_name, "bw") == 0,
               "positional module and subtest are recorded");
    check_true(cfg.cpu_id == 2 && cfg.qos_set && cfg.qos_perf, "cpu and qos perf are parsed");
    check_true(cfg.max_mem_mb == 128u && cfg.batch_ms == 50u && cfg.cool_ms == 10u,
               "numeric options are parsed");
    check_true(!cfg.no_thp && cfg.thp_set, "--thp overrides default no-thp");
    check_true(cfg.threads == 4u && cfg.csv && cfg.no_color, "--threads, --csv, --no-color parsed");

    check_true(parse_list(ctx_args, (int)(sizeof(ctx_args) / sizeof(ctx_args[0])), &cfg, &res) ==
                   LB_STATUS_OK,
               "os ctx --mode proc succeeds");
    check_true(res.mode_set && !res.mode_is_topo && cfg.os_ctx_mode == LB_OS_CTX_PROC,
               "--mode proc updates os_ctx_mode");

    check_true(parse_list(help_args, (int)(sizeof(help_args) / sizeof(help_args[0])), &cfg, &res) ==
                   LB_STATUS_OK,
               "--help with trailing --no-color succeeds");
    check_true(res.show_help && cfg.no_color, "--help continues scanning for --no-color");

    check_true(parse_list(ver_args, (int)(sizeof(ver_args) / sizeof(ver_args[0])), &cfg, &res) ==
                   LB_STATUS_OK,
               "-V succeeds");
    check_true(res.show_version, "-V sets show_version");
    tty_set_no_color(false);
}

static void test_invalid_flags(void)
{
    lb_cfg_t cfg = {0};
    lb_cli_result_t res = {0};
    const char *const bad_unknown[] = {"latbench", "--nope"};
    const char *const bad_cpu_neg[] = {"latbench", "sys", "info", "--cpu", "-1"};
    const char *const bad_cpu_txt[] = {"latbench", "sys", "info", "--cpu", "abc"};
    const char *const bad_qos[] = {"latbench", "sys", "info", "--qos", "fast"};
    const char *const bad_mode[] = {"latbench", "os", "ctx", "--mode", "bad"};
    const char *const bad_missing[] = {"latbench", "--cpu"};
    const char *const bad_threads_zero[] = {"latbench", "dram", "bw", "--threads", "0"};
    const char *const bad_threads_scope[] = {"latbench", "mem", "size", "--threads", "2"};
    const char *const bad_extra_pos[] = {"latbench", "sys", "info", "extra"};
    char dummy_arg[] = "latbench";
    char *dummy_argv[1] = {NULL};
    FILE *err_file = NULL;
    char err_buf[512] = {0};
    size_t err_bytes = 0u;

    dummy_argv[0] = dummy_arg;
    check_true(parse_list(bad_unknown, 2, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "unknown option is rejected");
    check_true(parse_list(bad_cpu_neg, 5, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "negative --cpu is rejected");
    check_true(parse_list(bad_cpu_txt, 5, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "non-numeric --cpu is rejected");
    check_true(parse_list(bad_qos, 5, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "invalid --qos is rejected");
    check_true(parse_list(bad_mode, 5, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "invalid --mode is rejected");
    check_true(parse_list(bad_missing, 2, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "option without value is rejected");
    check_true(parse_list(bad_threads_zero, 5, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "--threads 0 is rejected");
    check_true(parse_list(bad_threads_scope, 5, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "--threads outside dram bw is rejected");
    check_true(parse_list(bad_extra_pos, 4, &cfg, &res) == LB_STATUS_INVALID_ARG,
               "third positional argument is rejected");

    check_true(lb_cli_parse(1, NULL, &cfg, &res) == LB_STATUS_INVALID_ARG, "NULL argv is rejected");
    check_true(lb_cli_parse(1, dummy_argv, NULL, &res) == LB_STATUS_INVALID_ARG,
               "NULL cfg is rejected");
    check_true(lb_cli_parse(1, dummy_argv, &cfg, NULL) == LB_STATUS_INVALID_ARG,
               "NULL out is rejected");

    (void)fflush(stderr);
    err_file = fopen(TEST_CAPTURE_ERR, "r");
    if (err_file != NULL) {
        err_bytes = fread(err_buf, 1u, sizeof(err_buf) - 1u, err_file);
        err_buf[err_bytes] = '\0';
        (void)fclose(err_file);
    }
    check_true(strstr(err_buf, "[ERROR]") != NULL, "CLI parse errors emit [ERROR] to stderr");
}

static void test_lookup_and_modes(void)
{
    const lb_module_t *dram = lb_cli_find_module("dram");
    const lb_subtest_t *bw = lb_cli_find_subtest(dram, "bw");

    check_true(dram != NULL && bw != NULL && bw->run != NULL,
               "module and subtest lookup finds dram bw");
    check_true(lb_cli_find_module("nonexistent") == NULL, "unknown module returns NULL");
    check_true(lb_cli_find_module(NULL) == NULL, "NULL module returns NULL");
    check_true(lb_cli_find_subtest(dram, "nope") == NULL, "unknown subtest returns NULL");
    check_true(lb_cli_find_subtest(NULL, "bw") == NULL, "NULL module to find_subtest returns NULL");

    check_true(lb_cli_mode_valid("topo", "pingpong", true), "topo mode is valid for topo pingpong");
    check_true(!lb_cli_mode_valid("topo", "pingpong", false),
               "os mode is invalid for topo pingpong");
    check_true(lb_cli_mode_valid("os", "ctx", false), "os mode is valid for os ctx");
    check_true(!lb_cli_mode_valid("os", "ctx", true), "topo mode is invalid for os ctx");
    check_true(!lb_cli_mode_valid("mem", "size", true), "mode is invalid for other subtests");
    check_true(!lb_cli_mode_valid(NULL, "ctx", false), "NULL module is rejected by mode check");
}

int main(void)
{
    if (freopen(TEST_CAPTURE_ERR, "w", stderr) == NULL) {
        printf("FAIL: cannot redirect stderr for test_cli\n");
        return 1;
    }

    test_defaults();
    test_valid_flags();
    test_invalid_flags();
    test_lookup_and_modes();

    (void)fflush(stderr);
    (void)remove(TEST_CAPTURE_ERR);

    printf("test_cli: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
