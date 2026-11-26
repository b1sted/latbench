#!/bin/sh
# SPDX-License-Identifier: GPL-3.0
#
# Mechanical style checks that do not require clang tooling. POSIX sh only.

set -eu

fail=0

files=$(find include src tests -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null |
    LC_ALL=C sort)

if [ -z "$files" ]; then
    echo "style-check: no C sources or headers found" >&2
    exit 1
fi

license=${SPDX_LICENSE:-GPL-3.0}
spdx="/* SPDX-License-Identifier: $license */"
old_spdx='/* SPDX-License-Identifier: MIT */'
for f in $files; do
    if [ "$(sed -n '1p' "$f")" != "$spdx" ]; then
        echo "style-check: $f:1: first line must be '$spdx'" >&2
        fail=1
    fi
    if grep -Fq "$old_spdx" "$f"; then
        echo "style-check: $f: obsolete MIT SPDX identifier is forbidden" >&2
        fail=1
    fi
done

for f in $files; do
    case $f in
    *.h) ;;
    *) continue ;;
    esac

    if grep -nE '^[[:space:]]*#[[:space:]]*pragma[[:space:]]+once([[:space:]]|$)' "$f" \
        >/dev/null; then
        echo "style-check: $f: '#pragma once' is forbidden; use include guards" >&2
        fail=1
    fi

    if ! grep -qE '^[[:space:]]*#[[:space:]]*ifndef[[:space:]]+[A-Za-z0-9_]+' "$f"; then
        echo "style-check: $f: missing '#ifndef' include guard" >&2
        fail=1
    fi
done

forbidden='<unistd\.h>|<fcntl\.h>|<pthread\.h>|<sys/|<mach/|<stdatomic\.h>|<threads\.h>|__atomic_|__sync_|__asm__|(^|[^[:alnum:]_])typeof([^[:alnum:]_]|$)'
for f in $files; do
    case $f in
    src/core/* | src/bench/* | include/*) ;;
    *) continue ;;
    esac

    if [ "$f" = "include/pal/compiler.h" ]; then
        continue
    fi

    matches=$(grep -nE "$forbidden" "$f" || true)
    if [ -n "$matches" ]; then
        printf '%s\n' "$matches" | sed "s|^|style-check: $f:|" >&2
        echo "style-check: $f: OS-specific or compiler-extension construct is forbidden" >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "style-check: ok"
