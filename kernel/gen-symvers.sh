#!/bin/sh
# Generate nvidia.symvers so nvmuxk can be built against nvidia.ko's
# nvidia_get_rm_ops without the NVIDIA source tree.
#
# Building an out-of-tree module against another module's exported symbol needs
# that symbol in a Module.symvers-format file, or modpost reports it undefined.
# The CRC in that file is not decoration: on a CONFIG_MODVERSIONS kernel it is
# recorded in the module's __versions section and compared against the exporting
# module at load time. A hardcoded 0 therefore loads fine on a kernel built
# without MODVERSIONS and is rejected with "disagrees about version of symbol"
# on one built with it. So it is read from the installed nvidia.ko rather than
# assumed.
#
# With MODVERSIONS enabled, modpost emits the CRC as an absolute symbol named
# __crc_<symbol> whose value is the CRC. With it disabled no such symbol exists,
# and 0 is then the correct answer rather than a fallback.
#
# Usage: gen-symvers.sh [output-file] [kernel-release]
set -e

OUT=${1:-$(dirname -- "$0")/nvidia.symvers}
KVER=${2:-$(uname -r)}
SYM=nvidia_get_rm_ops
MOD=nvidia

find_module() {
    modinfo -k "$KVER" -n "$MOD" 2>/dev/null && return 0
    for d in extramodules updates kernel/drivers/video kernel/drivers/gpu/drm/nvidia; do
        for e in "" .zst .xz .gz; do
            p="/lib/modules/$KVER/$d/$MOD.ko$e"
            [ -f "$p" ] && { printf '%s\n' "$p"; return 0; }
        done
    done
    return 1
}

decompress_to() {
    case "$1" in
        *.zst) zstd -dcq   -- "$1" > "$2" ;;
        *.xz)  xz   -dc    -- "$1" > "$2" ;;
        *.gz)  gzip -dc    -- "$1" > "$2" ;;
        *)     cat         -- "$1" > "$2" ;;
    esac
}

crc=0x00000000
if KO=$(find_module); then
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT INT TERM
    if decompress_to "$KO" "$TMP/$MOD.ko" 2>/dev/null; then
        # Field 1 is the symbol value, which for __crc_* is the CRC itself.
        v=$(nm -- "$TMP/$MOD.ko" 2>/dev/null \
            | awk -v s="__crc_$SYM" '$NF == s { print $1; exit }')
        if [ -n "$v" ]; then
            crc="0x$(printf '%s' "$v" | tr 'A-F' 'a-f' | sed 's/^0*\(........\)$/\1/')"
            echo "gen-symvers: $SYM CRC $crc (from $KO)" >&2
        else
            echo "gen-symvers: $KO exports no __crc_$SYM; kernel is built without MODVERSIONS, CRC 0 is correct" >&2
        fi
    fi
    # Refuse to guess if the module is there but does not export what we need.
    if ! nm -- "$TMP/$MOD.ko" 2>/dev/null | grep -q "__ksymtab_$SYM"; then
        echo "gen-symvers: WARNING - $KO does not export $SYM." >&2
        echo "gen-symvers: nvmuxk needs the NVIDIA open kernel modules; the" >&2
        echo "gen-symvers: proprietary-only build does not export it." >&2
    fi
else
    echo "gen-symvers: WARNING - no $MOD.ko found for kernel $KVER." >&2
    echo "gen-symvers: writing CRC 0; the module will build but may not load." >&2
fi

printf '%s\t%s\t%s\tEXPORT_SYMBOL\t\n' "$crc" "$SYM" "$MOD" > "$OUT"
echo "gen-symvers: wrote $OUT" >&2
