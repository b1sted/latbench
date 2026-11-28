/* SPDX-License-Identifier: GPL-3.0 */
#ifndef PAL_COMPILER_H
#define PAL_COMPILER_H

/*
 * The only header allowed to expose compiler-specific primitives. Portable
 * code includes this header and never spells GNU/Clang builtins directly.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * Unaligned alias types for hardware probes that intentionally cross an
 * alignment boundary. GCC and Clang accept the attribute; other compilers
 * fall back to the plain fixed-width type.
 */
#if defined(__GNUC__) || defined(__clang__)
typedef uint64_t pal_unaligned_u64_t __attribute__((aligned(1), may_alias));
typedef uint32_t pal_unaligned_u32_t __attribute__((aligned(1), may_alias));
typedef uint8_t pal_unaligned_u8_t __attribute__((aligned(1), may_alias));
#else
typedef uint64_t pal_unaligned_u64_t;
typedef uint32_t pal_unaligned_u32_t;
typedef uint8_t pal_unaligned_u8_t;
#endif

/* Prevent the compiler from inlining a helper into a timed benchmark loop. */
#if defined(__GNUC__) || defined(__clang__)
#define PAL_NOINLINE __attribute__((noinline))
#else
#define PAL_NOINLINE
#endif

/*
 * Force a tiny helper inline even when the caller is marked with an
 * optimization-disabling attribute such as Clang's optnone.
 */
#if defined(__GNUC__) || defined(__clang__)
#define PAL_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define PAL_ALWAYS_INLINE inline
#endif

/*
 * Hint minimum alignment in bytes to the optimizer. Only use on pointers that
 * genuinely satisfy that alignment at runtime, or UBSan checks will fail.
 */
#if defined(__GNUC__) || defined(__clang__)
#define PAL_ASSUME_ALIGNED(ptr, align) __builtin_assume_aligned((ptr), (align))
#else
#define PAL_ASSUME_ALIGNED(ptr, align) (ptr)
#endif

/*
 * Portable printf-style format checking for variadic wrappers. Pass the
 * one-based index of the format string and of the first variadic argument, or
 * zero as the second index for a va_list parameter.
 */
#if defined(__GNUC__) || defined(__clang__)
#define PAL_PRINTF_FORMAT(format_index, first_arg)                                                 \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define PAL_PRINTF_FORMAT(format_index, first_arg)
#endif

/*
 * Force a real basic-block edge. This stops the optimizer from turning a
 * short if/else into a conditional move (cmov/csel) that has different
 * latency characteristics than the branch being measured.
 *
 * The two branch-specific variants carry distinct marker strings on purpose:
 * LLVM SimplifyCFG merges identical empty barriers and then if-converts the
 * whole if/else into a cmov. Different asm text keeps the taken and not-taken
 * blocks distinct, so both Clang and GCC emit a real conditional branch.
 */
#if defined(__GNUC__) || defined(__clang__)
#define PAL_BLOCK_IFCONV() __asm__ volatile("" ::: "memory")
#define PAL_BLOCK_IFCONV_T() __asm__ volatile("/* br_t */")
#define PAL_BLOCK_IFCONV_F() __asm__ volatile("/* br_f */")
#define PAL_BARRIER() __asm__ volatile("" ::: "memory")
#else
#define PAL_BLOCK_IFCONV() ((void)0)
#define PAL_BLOCK_IFCONV_T() ((void)0)
#define PAL_BLOCK_IFCONV_F() ((void)0)
#define PAL_BARRIER() ((void)0)
#endif

/*
 * Function-level auto-vectorization suppression. GCC has a dedicated
 * optimization attribute. Clang has no vectorize-only function attribute, and
 * optnone would compile the whole function at -O0 (spilling accumulators,
 * dropping fmadd, and turning small memcpy() calls into real libc calls), so
 * Clang relies solely on PAL_NO_VECTORIZE_LOOP placed directly before each
 * hot loop.
 */
#if defined(__clang__)
#define PAL_NO_VECTORIZE
#define PAL_NO_VECTORIZE_LOOP _Pragma("clang loop vectorize(disable) interleave(disable)")
#elif defined(__GNUC__)
#define PAL_NO_VECTORIZE __attribute__((optimize("no-tree-vectorize")))
#if (__GNUC__ >= 14)
#define PAL_NO_VECTORIZE_LOOP _Pragma("GCC novector")
#else
#define PAL_NO_VECTORIZE_LOOP
#endif
#else
#define PAL_NO_VECTORIZE
#define PAL_NO_VECTORIZE_LOOP
#endif

/*
 * Keep a modifiable integer or floating-point (float/double) lvalue alive in a
 * register. Both wrappers are statement macros.
 */
#if defined(__GNUC__) || defined(__clang__)
#define PAL_KEEP_INT(x) __asm__ volatile("" : "+r"(x))
#else
#define PAL_KEEP_INT(x) ((void)(x))
#endif

#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__)
#define PAL_KEEP_FP(x) __asm__ volatile("" : "+x"(x))
#elif defined(__i386__) && defined(__SSE__)
#define PAL_KEEP_FP(x) __asm__ volatile("" : "+x"(x))
#elif defined(__i386__)
#define PAL_KEEP_FP(x) __asm__ volatile("" : "+f"(x))
#elif defined(__aarch64__) || (defined(__arm__) && defined(__ARM_FP))
#define PAL_KEEP_FP(x) __asm__ volatile("" : "+w"(x))
#elif defined(__riscv) && defined(__riscv_flen)
#define PAL_KEEP_FP(x) __asm__ volatile("" : "+f"(x))
#else
#define PAL_KEEP_FP(x)                                                                             \
    do {                                                                                           \
        volatile long double pal_keep_fp_tmp_ = (long double)(x);                                  \
        (void)pal_keep_fp_tmp_;                                                                    \
    } while (0)
#endif
#else
#define PAL_KEEP_FP(x)                                                                             \
    do {                                                                                           \
        volatile long double pal_keep_fp_tmp_ = (long double)(x);                                  \
        (void)pal_keep_fp_tmp_;                                                                    \
    } while (0)
#endif

/*
 * Sequential-consistency atomics. GCC 4.7+ and Clang provide __atomic_*; the
 * __sync_* fallback keeps very old GCC toolchains working.
 */
#if defined(__ATOMIC_SEQ_CST)
#define PAL_HAVE_ATOMIC_BUILTINS 1
#else
#define PAL_HAVE_ATOMIC_BUILTINS 0
#endif

static inline uint32_t pal_atomic_load_u32(const uint32_t *ptr)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
#else
    /* __sync_* has no load primitive; a volatile read followed by a barrier. */
    const volatile uint32_t *addr = ptr;
    const uint32_t value = *addr;

    __sync_synchronize();
    return value;
#endif
}

static inline void pal_atomic_store_u32(uint32_t *ptr, uint32_t value)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
#else
    __sync_synchronize();
    (void)__sync_lock_test_and_set(ptr, value);
    __sync_synchronize();
#endif
}

static inline uint64_t pal_atomic_fetch_add_u64(uint64_t *ptr, uint64_t value)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    return __atomic_fetch_add(ptr, value, __ATOMIC_SEQ_CST);
#else
    return (uint64_t)__sync_fetch_and_add(ptr, value);
#endif
}

static inline uint32_t pal_atomic_fetch_add_u32(uint32_t *ptr, uint32_t value)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    return __atomic_fetch_add(ptr, value, __ATOMIC_SEQ_CST);
#else
    return (uint32_t)__sync_fetch_and_add(ptr, value);
#endif
}

static inline bool pal_atomic_cas_u32(uint32_t *ptr, uint32_t expected, uint32_t desired)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    uint32_t exp = expected;
    return __atomic_compare_exchange_n(ptr, &exp, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
#else
    return __sync_bool_compare_and_swap(ptr, expected, desired);
#endif
}

/*
 * Relaxed atomics for benchmark token handoff. They carry no payload that
 * needs ordering, so the acquire/release fences of the sequential-consistency
 * wrappers above would only distort core-to-core latency. On x86-64 these
 * compile to plain loads and stores instead of locked instructions.
 */
static inline uint32_t pal_atomic_load_u32_relaxed(const uint32_t *ptr)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    return __atomic_load_n(ptr, __ATOMIC_RELAXED);
#else
    const volatile uint32_t *addr = ptr;

    return *addr;
#endif
}

static inline void pal_atomic_store_u32_relaxed(uint32_t *ptr, uint32_t value)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    __atomic_store_n(ptr, value, __ATOMIC_RELAXED);
#else
    volatile uint32_t *addr = ptr;

    *addr = value;
#endif
}

static inline bool pal_atomic_cas_u32_relaxed(uint32_t *ptr, uint32_t expected, uint32_t desired)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    uint32_t exp = expected;
    return __atomic_compare_exchange_n(ptr, &exp, desired, false, __ATOMIC_RELAXED,
                                       __ATOMIC_RELAXED);
#else
    return __sync_bool_compare_and_swap(ptr, expected, desired);
#endif
}

static inline void pal_atomic_fence_seq(void)
{
#if PAL_HAVE_ATOMIC_BUILTINS
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
    __sync_synchronize();
#endif
}

#endif /* PAL_COMPILER_H */
