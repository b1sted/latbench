/* SPDX-License-Identifier: GPL-3.0 */

#include "auto.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "defs.h"
#include "pal/pal.h"
#include "passport.h"
#include "tty.h"
#include "types.h"

typedef struct lb_auto_probe_t {
    const char *module;
    const char *name;
    lb_subtest_fn run;
} lb_auto_probe_t;

/*
 * Key probes only. The list is ordered from calibration through the OS layer so
 * the passport reads like a machine summary and each stage warms the next.
 */
static const lb_auto_probe_t AUTO_PROBES[] = {
    {"sys", "info", sys_info_run},
    {"sys", "timer", sys_timer_run},
    {"mem", "size", mem_size_run},
    {"mem", "assoc", mem_assoc_run},
    {"mem", "prefetch", mem_prefetch_run},
    {"tlb", "size", tlb_size_run},
    {"tlb", "fault", tlb_fault_run},
    {"core", "ilp", core_ilp_run},
    {"core", "mlp", core_mlp_run},
    {"core", "align", core_align_run},
    {"core", "stlf", core_stlf_run},
    {"branch", "hist", br_hist_run},
    {"branch", "rsb", br_rsb_run},
    {"branch", "btb", br_btb_run},
    {"dram", "bw", dram_bw_run},
    {"dram", "row", dram_row_run},
    {"topo", "atomic", topo_atomic_run},
    {"topo", "falseshare", topo_falseshare_run},
    {"topo", "pingpong", topo_pingpong_run},
    {"os", "syscall", os_syscall_run},
    {"os", "ctx", os_ctx_run},
    {"os", "wakeup", os_wakeup_run},
    {"os", "shootdown", os_shootdown_run},
    {"os", "fork", os_fork_run},
    {"os", "sig", os_sig_run},
    {"os", "sleep", os_sleep_run},
};

static const char *auto_text(const char *text, const char *fallback)
{
    return (text != NULL && text[0] != '\0') ? text : fallback;
}

static void auto_print_passport(const lb_cfg_t *cfg, const lb_passport_t *p)
{
    char page[LB_NAME_MAX] = {0};
    char ram[LB_NAME_MAX] = {0};
    char l1[LB_NAME_MAX] = {0};
    char l2[LB_NAME_MAX] = {0};
    char l3[LB_NAME_MAX] = {0};
    char cache[LB_LINE_MAX] = {0};

    tty_section(cfg, "Passport");

    tty_header(cfg, "  CPU/OS");
    tty_kv(cfg, "Arch", "%s", auto_text(p->arch, "unknown"));
    if (p->page_size > 0u) {
        (void)tty_format_bytes((uint64_t)p->page_size, page, sizeof(page));
        tty_kv(cfg, "Page size", "%s (%zu bytes)", page, p->page_size);
    } else {
        tty_kv(cfg, "Page size", "unknown");
    }
    if (p->ram_bytes > 0u) {
        (void)tty_format_bytes(p->ram_bytes, ram, sizeof(ram));
        tty_kv(cfg, "RAM", "%s", ram);
    } else {
        tty_kv(cfg, "RAM", "unknown");
    }
    tty_kv(cfg, "Logical CPUs", "%" PRIu32, p->cpu_count);

    tty_header(cfg, "  Cache/TLB");
    if (p->l1d_bytes > 0u) {
        (void)tty_format_bytes(p->l1d_bytes, l1, sizeof(l1));
        tty_kv(cfg, "L1d est.", "~%s", l1);
    } else {
        tty_kv(cfg, "L1d est.", "not detected");
    }
    if (p->l1d_ways > 0u) {
        tty_kv(cfg, "L1d ways", "%" PRIu32, p->l1d_ways);
    } else {
        tty_kv(cfg, "L1d ways", "not detected");
    }
    (void)tty_format_bytes(p->l2_bytes, l2, sizeof(l2));
    (void)tty_format_bytes(p->l3_bytes, l3, sizeof(l3));
    if (p->l2_bytes > 0u && p->l3_bytes > 0u) {
        (void)snprintf(cache, sizeof(cache), "~%s / ~%s", l2, l3);
    } else if (p->l2_bytes > 0u) {
        (void)snprintf(cache, sizeof(cache), "~%s / not detected", l2);
    } else if (p->l3_bytes > 0u) {
        (void)snprintf(cache, sizeof(cache), "not detected / ~%s", l3);
    } else {
        (void)snprintf(cache, sizeof(cache), "not detected");
    }
    tty_kv(cfg, "L2/L3 est.", "%s", cache);
    if (p->coherence_line_bytes > 0u) {
        tty_kv(cfg, "Coherence line", "~%" PRIu32 " B", p->coherence_line_bytes);
    } else {
        tty_kv(cfg, "Coherence line", "not detected");
    }
    if (p->l1dtlb_entries > 0u) {
        char tlb[LB_LINE_MAX] = {0};
        const uint64_t bytes = (uint64_t)p->l1dtlb_entries * (uint64_t)p->page_size;

        (void)tty_format_bytes(bytes, l1, sizeof(l1));
        (void)snprintf(tlb, sizeof(tlb), "~%" PRIu32 " entries (%s)", p->l1dtlb_entries, l1);
        tty_kv(cfg, "L1 dTLB est.", "%s", tlb);
    } else {
        tty_kv(cfg, "L1 dTLB est.", "not detected");
    }

    tty_header(cfg, "  Core/Branch");
    if (p->alu_ilp_chains > 0u && p->fpu_ilp_chains > 0u) {
        char ilp[LB_LINE_MAX] = {0};

        (void)snprintf(ilp, sizeof(ilp), "%" PRIu32 " chains (%.1fx) / %" PRIu32 " chains (%.1fx)",
                       p->alu_ilp_chains, p->alu_ilp_speedup, p->fpu_ilp_chains,
                       p->fpu_ilp_speedup);
        tty_kv(cfg, "ALU/FPU ILP", "%s", ilp);
    } else {
        tty_kv(cfg, "ALU/FPU ILP", "not detected");
    }
    if (p->stlf_stall_ns > 0.0) {
        tty_kv(cfg, "STLF stall", "%.2f ns (4B @ +1)", p->stlf_stall_ns);
    } else {
        tty_kv(cfg, "STLF stall", "not detected");
    }
    if (p->rsb_frames > 0u) {
        tty_kv(cfg, "RSB est.", "~%" PRIu64 " frames", p->rsb_frames);
    } else {
        tty_kv(cfg, "RSB est.", "not detected");
    }
    if (p->btb_targets > 0u) {
        tty_kv(cfg, "BTB est.", "~%" PRIu64 " targets", p->btb_targets);
    } else {
        tty_kv(cfg, "BTB est.", "not detected");
    }
    if (p->mispredict_ns > 0.0) {
        tty_kv(cfg, "Branch mispredict", "~%.2f ns", p->mispredict_ns);
    } else {
        tty_kv(cfg, "Branch mispredict", "below noise");
    }

    tty_header(cfg, "  DRAM/Topo");
    if (p->dram_read_gibs > 0.0 || p->dram_write_gibs > 0.0) {
        tty_kv(cfg, "Read/Write BW", "%.2f / %.2f GiB/s", p->dram_read_gibs, p->dram_write_gibs);
    } else {
        tty_kv(cfg, "Read/Write BW", "not detected");
    }
    if (p->row_delta_ns != 0.0 || p->row_delta_pct != 0.0) {
        tty_kv(cfg, "Row conflict delta", "%+.2f ns (%+.1f%%)", p->row_delta_ns, p->row_delta_pct);
    } else {
        tty_kv(cfg, "Row conflict delta", "not detected");
    }
    if (p->pp_max_ns > 0u) {
        tty_kv(cfg, "Core-to-core min/med/max", "%" PRIu64 " / %" PRIu64 " / %" PRIu64 " ns",
               p->pp_min_ns, p->pp_med_ns, p->pp_max_ns);
    } else {
        tty_kv(cfg, "Core-to-core min/med/max", "skipped (single CPU)");
    }

    tty_header(cfg, "  OS");
    if (p->os_syscall_ns > 0.0) {
        tty_kv(cfg, "Syscall", "%.2f ns/call", p->os_syscall_ns);
    } else {
        tty_kv(cfg, "Syscall", "not detected");
    }
    if (p->os_ctx_thread_us > 0.0 || p->os_ctx_proc_us > 0.0) {
        tty_kv(cfg, "Ctx thread/proc", "%.3f / %.3f us", p->os_ctx_thread_us, p->os_ctx_proc_us);
    } else {
        tty_kv(cfg, "Ctx thread/proc", "not detected");
    }
    if (p->os_wakeup_us > 0.0) {
        tty_kv(cfg, "Wakeup", "%.3f us", p->os_wakeup_us);
    } else {
        tty_kv(cfg, "Wakeup", "not detected");
    }
    if (p->os_shootdown_single_us > 0.0) {
        if (p->os_shootdown_multi_us > 0.0) {
            tty_kv(cfg, "Shootdown 1T/MT", "%.3f / %.3f us", p->os_shootdown_single_us,
                   p->os_shootdown_multi_us);
        } else {
            tty_kv(cfg, "Shootdown 1T/MT", "%.3f us / n/a", p->os_shootdown_single_us);
        }
    } else {
        tty_kv(cfg, "Shootdown 1T/MT", "not detected");
    }
    if (p->os_fork_us > 0.0) {
        tty_kv(cfg, "Fork", "%.2f us", p->os_fork_us);
    } else {
        tty_kv(cfg, "Fork", "not detected");
    }
    if (p->os_sleep_1us_p50_us > 0.0) {
        tty_kv(cfg, "Sleep 1us", "p50 %.2f us", p->os_sleep_1us_p50_us);
    } else {
        tty_kv(cfg, "Sleep 1us", "not detected");
    }
}

static void auto_publish_csv(const lb_cfg_t *cfg, const lb_passport_t *p)
{
    tty_csv_row(cfg, "module,subtest,metric,value,unit");
    tty_csv_row(cfg, "auto,passport,arch,%s,text", auto_text(p->arch, "unknown"));
    tty_csv_row(cfg, "auto,passport,page_size,%zu,bytes", p->page_size);
    tty_csv_row(cfg, "auto,passport,ram,%" PRIu64 ",bytes", p->ram_bytes);
    tty_csv_row(cfg, "auto,passport,cpu_count,%" PRIu32 ",count", p->cpu_count);
    tty_csv_row(cfg, "auto,passport,l1d,%" PRIu64 ",bytes", p->l1d_bytes);
    tty_csv_row(cfg, "auto,passport,l1d_ways,%" PRIu32 ",ways", p->l1d_ways);
    tty_csv_row(cfg, "auto,passport,l2,%" PRIu64 ",bytes", p->l2_bytes);
    tty_csv_row(cfg, "auto,passport,l3,%" PRIu64 ",bytes", p->l3_bytes);
    tty_csv_row(cfg, "auto,passport,coherence_line,%" PRIu32 ",bytes", p->coherence_line_bytes);
    tty_csv_row(cfg, "auto,passport,l1dtlb,%" PRIu32 ",entries", p->l1dtlb_entries);
    tty_csv_row(cfg, "auto,passport,alu_ilp_chains,%" PRIu32 ",chains", p->alu_ilp_chains);
    tty_csv_row(cfg, "auto,passport,alu_ilp_speedup,%.3f,x", p->alu_ilp_speedup);
    tty_csv_row(cfg, "auto,passport,fpu_ilp_chains,%" PRIu32 ",chains", p->fpu_ilp_chains);
    tty_csv_row(cfg, "auto,passport,fpu_ilp_speedup,%.3f,x", p->fpu_ilp_speedup);
    tty_csv_row(cfg, "auto,passport,stlf_stall,%.3f,ns", p->stlf_stall_ns);
    tty_csv_row(cfg, "auto,passport,rsb,%" PRIu64 ",frames", p->rsb_frames);
    tty_csv_row(cfg, "auto,passport,btb,%" PRIu64 ",targets", p->btb_targets);
    tty_csv_row(cfg, "auto,passport,mispredict,%.3f,ns", p->mispredict_ns);
    tty_csv_row(cfg, "auto,passport,dram_read,%.3f,gib_per_s", p->dram_read_gibs);
    tty_csv_row(cfg, "auto,passport,dram_write,%.3f,gib_per_s", p->dram_write_gibs);
    tty_csv_row(cfg, "auto,passport,row_delta,%.3f,ns", p->row_delta_ns);
    tty_csv_row(cfg, "auto,passport,row_delta_pct,%.3f,percent", p->row_delta_pct);
    tty_csv_row(cfg, "auto,passport,pingpong_min,%" PRIu64 ",ns", p->pp_min_ns);
    tty_csv_row(cfg, "auto,passport,pingpong_med,%" PRIu64 ",ns", p->pp_med_ns);
    tty_csv_row(cfg, "auto,passport,pingpong_max,%" PRIu64 ",ns", p->pp_max_ns);
    tty_csv_row(cfg, "auto,passport,syscall,%.3f,ns", p->os_syscall_ns);
    tty_csv_row(cfg, "auto,passport,ctx_thread,%.3f,us", p->os_ctx_thread_us);
    tty_csv_row(cfg, "auto,passport,ctx_proc,%.3f,us", p->os_ctx_proc_us);
    tty_csv_row(cfg, "auto,passport,wakeup,%.3f,us", p->os_wakeup_us);
    tty_csv_row(cfg, "auto,passport,shootdown_single,%.3f,us", p->os_shootdown_single_us);
    tty_csv_row(cfg, "auto,passport,shootdown_multi,%.3f,us", p->os_shootdown_multi_us);
    tty_csv_row(cfg, "auto,passport,fork,%.3f,us", p->os_fork_us);
    tty_csv_row(cfg, "auto,passport,sleep_1us_p50,%.3f,us", p->os_sleep_1us_p50_us);
}

lb_status_t lb_auto_run(const lb_cfg_t *cfg)
{
    lb_cfg_t run = {0};
    lb_cfg_t render = {0};
    lb_passport_t *passport = NULL;
    uint32_t failures = 0u;
    uint64_t begin = 0u;
    uint64_t end = 0u;
    size_t index = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    run = *cfg;
    run.fast = true;
    run.auto_mode = true;

    lb_passport_reset();
    passport = lb_passport_mutable();
    passport->arch = pal_arch_name();
    passport->page_size = pal_page_size();
    passport->ram_bytes = pal_total_ram_bytes();
    passport->cpu_count = pal_cpu_count();

    begin = pal_now_ns();
    for (index = 0u; index < LB_ARRAY_SIZE(AUTO_PROBES); index++) {
        lb_status_t status = LB_STATUS_OK;

        if (strcmp(AUTO_PROBES[index].module, "topo") == 0 && passport->cpu_count < 2u) {
            tty_progress(&run, "auto: %s %s ... skipped (single CPU)", AUTO_PROBES[index].module,
                         AUTO_PROBES[index].name);
            continue;
        }

        tty_progress(&run, "auto: %s %s ...", AUTO_PROBES[index].module, AUTO_PROBES[index].name);
        status = AUTO_PROBES[index].run(&run);
        if (status != LB_STATUS_OK) {
            failures++;
            tty_warn("auto: %s %s failed (status %d)", AUTO_PROBES[index].module,
                     AUTO_PROBES[index].name, (int)status);
        }
    }
    end = pal_now_ns();
    tty_progress(&run, "auto: %zu probes in %.2f s (%" PRIu32 " failed)",
                 LB_ARRAY_SIZE(AUTO_PROBES), (double)(end - begin) / 1.0e9, failures);

    if (cfg->csv) {
        auto_publish_csv(&run, passport);
    } else if (!cfg->quiet) {
        render = *cfg;
        render.csv = false;
        render.auto_mode = false;
        render.quiet = false;
        auto_print_passport(&render, passport);
    }

    return LB_STATUS_OK;
}
