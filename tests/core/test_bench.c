/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"

static int g_checks = 0;
static int g_failures = 0;

static void check_true(bool condition, const char *name)
{
    g_checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        g_failures++;
    }
}

static void test_mem_line_estimator(void)
{
    static const size_t STEPS[6] = {8u, 16u, 32u, 64u, 128u, 256u};
    static const double RISCV_64B[6] = {179.7, 178.5, 177.6, 305.2, 307.2, 320.0};
    static const double M1_128B[6] = {130.3, 120.0, 109.1, 125.6, 214.2, 227.9};
    static const double LINE_32B[6] = {10.0, 10.0, 20.0, 20.5, 20.2, 20.1};
    static const double FLAT[6] = {10.0, 10.1, 10.0, 10.2, 10.1, 10.0};
    static const double SPIKE[6] = {10.0, 10.0, 10.0, 15.0, 10.0, 10.0};

    check_true(mem_line_estimate_p10(RISCV_64B, STEPS, 6u) == 64u,
               "mem_line detects 64 B cache line");
    check_true(mem_line_estimate_p10(M1_128B, STEPS, 6u) == 128u,
               "mem_line detects 128 B cache line");
    check_true(mem_line_estimate_p10(LINE_32B, STEPS, 6u) == 32u,
               "mem_line detects 32 B cache line");
    check_true(mem_line_estimate_p10(FLAT, STEPS, 6u) == 0u, "mem_line rejects flat latency curve");
    check_true(mem_line_estimate_p10(SPIKE, STEPS, 6u) == 0u,
               "mem_line rejects single unsustained spike");
    check_true(mem_line_estimate_p10(NULL, STEPS, 6u) == 0u, "mem_line rejects NULL latencies");
    check_true(mem_line_estimate_p10(RISCV_64B, NULL, 6u) == 0u, "mem_line rejects NULL steps");
    check_true(mem_line_estimate_p10(RISCV_64B, STEPS, 0u) == 0u, "mem_line rejects zero count");
}

static void test_mem_assoc_estimator(void)
{
    double row[16] = {0.0};
    double ratio = 0.0;
    size_t i = 0u;

    for (i = 0u; i < 16u; i++) {
        row[i] = (i < 8u) ? 1.0 : 5.0;
    }
    check_true(mem_assoc_knee(row, 16u, &ratio) == 9u && ratio > 4.5,
               "mem_assoc detects 8-way knee at N=9");

    for (i = 0u; i < 16u; i++) {
        row[i] = (i < 4u) ? 1.0 : 3.2;
    }
    check_true(mem_assoc_knee(row, 16u, &ratio) == 5u && ratio > 3.0,
               "mem_assoc detects 4-way knee at N=5");

    for (i = 0u; i < 16u; i++) {
        row[i] = (i < 2u) ? 1.0 : 4.0;
    }
    check_true(mem_assoc_knee(row, 16u, &ratio) == 3u && ratio > 3.5,
               "mem_assoc detects 2-way knee at N=3");

    for (i = 0u; i < 16u; i++) {
        row[i] = 1.0;
    }
    row[4] = 6.0;
    check_true(mem_assoc_knee(row, 16u, &ratio) == 0u, "mem_assoc rejects single-point spike");
    check_true(mem_assoc_knee(NULL, 16u, &ratio) == 0u, "mem_assoc rejects NULL row");
    check_true(mem_assoc_knee(row, 3u, &ratio) == 0u, "mem_assoc rejects short row");
}

static void test_topo_fs_estimator(void)
{
    static const size_t OFFSETS[9] = {0u, 8u, 16u, 32u, 64u, 96u, 128u, 192u, 256u};
    static const double INTEL_64B[9] = {29.92, 29.74, 29.79, 29.99, 3.44, 3.44, 3.44, 3.44, 3.43};
    static const double M1_128B[9] = {42.38, 27.88, 39.33, 35.03, 12.91, 11.76, 8.75, 9.46, 10.66};
    static const double RISCV_64B[9] = {62.90, 62.78, 62.83, 62.82, 58.50,
                                        58.78, 58.85, 58.50, 58.41};
    static const double HT_NOISE[9] = {6.10, 6.05, 6.12, 6.08, 5.95, 6.00, 5.98, 6.02, 5.96};
    static const double HIGH_FLAT[9] = {60.0, 59.5, 60.2, 59.8, 59.2, 59.6, 59.4, 59.7, 59.1};
    static const double DRIFT[9] = {74.0, 72.0, 70.0, 68.0, 64.0, 62.0, 61.0, 60.0, 60.0};

    check_true(topo_fs_estimate_line_offsets(OFFSETS, INTEL_64B, 9u) == 64u,
               "topo_fs detects 64 B coherence line on fast atomics");
    check_true(topo_fs_estimate_line_offsets(OFFSETS, M1_128B, 9u) == 128u,
               "topo_fs detects 128 B coherence line");
    check_true(topo_fs_estimate_line_offsets(OFFSETS, RISCV_64B, 9u) == 64u,
               "topo_fs detects subtle 64 B plateau drop on expensive atomics");
    check_true(topo_fs_estimate_line_offsets(OFFSETS, HT_NOISE, 9u) == 0u,
               "topo_fs ignores SMT flat noise");
    check_true(topo_fs_estimate_line_offsets(OFFSETS, HIGH_FLAT, 9u) == 0u,
               "topo_fs ignores high-latency flat noise");
    check_true(topo_fs_estimate_line_offsets(OFFSETS, DRIFT, 9u) == 0u,
               "topo_fs rejects smooth downward DVFS drift");
    check_true(topo_fs_estimate_line_offsets(NULL, INTEL_64B, 9u) == 0u,
               "topo_fs rejects NULL offsets");
    check_true(topo_fs_estimate_line_offsets(OFFSETS, NULL, 9u) == 0u,
               "topo_fs rejects NULL samples");
    check_true(topo_fs_estimate_line_offsets(OFFSETS, INTEL_64B, 0u) == 0u,
               "topo_fs rejects zero count");
}

static void test_br_rsb_estimator(void)
{
    static const uint64_t DEPTHS[12] = {4u, 8u, 12u, 16u, 20u, 24u, 32u, 48u, 56u, 64u, 80u, 96u};
    static const double INTEL_20[12] = {0.8415, 0.7387, 0.7474, 0.7322, 0.7686, 1.3703,
                                        2.3629, 3.7338, 3.9804, 4.1675, 4.4650, 4.6427};
    static const double SM8650Q_32[12] = {0.7911, 0.7435, 0.7082, 0.7419, 0.7083, 0.7132,
                                          0.7055, 0.9960, 0.9898, 0.9851, 1.0110, 1.0214};
    static const double M1_48[12] = {0.9226, 0.9051, 1.0049, 1.0440, 0.9995, 0.9979,
                                     1.6093, 0.9485, 5.9192, 5.2468, 4.2164, 4.3295};
    static const double RISCV_16[12] = {5.8774, 5.2810, 5.5858, 6.4191, 7.1116, 7.4288,
                                        8.4083, 9.7899, 8.9203, 9.2226, 9.1101, 9.5869};
    static const double FLAT[12] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    size_t base_index = 0u;
    size_t threshold_index = 0u;
    bool found = false;

    found = br_rsb_detect_threshold(DEPTHS, INTEL_20, 12u, &base_index, &threshold_index);
    check_true(found && threshold_index > 0u && DEPTHS[threshold_index - 1u] == 20u,
               "br_rsb detects 20-frame RSB on Intel curve");

    found = br_rsb_detect_threshold(DEPTHS, SM8650Q_32, 12u, &base_index, &threshold_index);
    check_true(found && threshold_index > 0u && DEPTHS[threshold_index - 1u] == 32u,
               "br_rsb detects 32-frame RSB on SM8650Q curve");

    found = br_rsb_detect_threshold(DEPTHS, M1_48, 12u, &base_index, &threshold_index);
    check_true(found && threshold_index > 0u && DEPTHS[threshold_index - 1u] == 48u,
               "br_rsb detects 48-frame RSB on M1 curve despite spike at 32");

    found = br_rsb_detect_threshold(DEPTHS, RISCV_16, 12u, &base_index, &threshold_index);
    check_true(found && threshold_index > 0u && DEPTHS[threshold_index - 1u] == 16u,
               "br_rsb detects 16-frame RSB on RISC-V C910 curve");

    found = br_rsb_detect_threshold(DEPTHS, FLAT, 12u, &base_index, &threshold_index);
    check_true(!found, "br_rsb reports no threshold on flat curve");

    check_true(!br_rsb_detect_threshold(NULL, FLAT, 12u, &base_index, &threshold_index),
               "br_rsb rejects NULL depths");
    check_true(!br_rsb_detect_threshold(DEPTHS, NULL, 12u, &base_index, &threshold_index),
               "br_rsb rejects NULL latencies");
    check_true(!br_rsb_detect_threshold(DEPTHS, FLAT, 1u, &base_index, &threshold_index),
               "br_rsb rejects count < 2");
}

int main(void)
{
    test_mem_line_estimator();
    test_mem_assoc_estimator();
    test_topo_fs_estimator();
    test_br_rsb_estimator();

    printf("test_bench: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
