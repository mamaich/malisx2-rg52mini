#!/bin/bash
# build.sh - mtrace.so for the console (aarch64): mtrace.c and Mesa's pandecode (decode.c,
# decode_jm.c for v7, decode_common.c) from this tree, with the compile flags and static libraries
# of a cross build of it without LTO ($BUILD, default <tree>/build-dev; docs/building.md).
# pandecode's mprotect of the mappings it reads is compiled out: the driver's other threads keep
# writing them while a chain is decoded. Output: $BUILD/mtrace.so
set -e
H=$(dirname "$(readlink -f "$0")")
S=$(readlink -f "$H/../..")
B=${BUILD:-$S/build-dev}
cd $B
FLAGS=$(python3 - <<'PY'
import json, shlex
for c in json.load(open("compile_commands.json")):
    if c["file"].endswith("pan_format.c") and "v7" in c["output"]:
        a = shlex.split(c["command"])[1:]
        out, skip = [], False
        for x in a:
            if skip:
                skip = False; continue
            if x in ("-o", "-c", "-MF", "-MQ"):
                skip = True; continue
            if x.endswith(".c") or x == "-MD" or x.startswith("-DPAN_ARCH") or ".a.p" in x:
                continue
            out.append(x)
        print(" ".join(shlex.quote(x) for x in out))
        break
PY
)
O=$B/mtrace-obj; mkdir -p $O
G=$S/mesa/src/panfrost/genxml
eval clang-19 $FLAGS -DPAN_ARCH=7 -c $G/decode.c -o $O/decode_v7.o
eval clang-19 $FLAGS -DPAN_ARCH=7 -c $G/decode_jm.c -o $O/decode_jm_v7.o
eval clang-19 $FLAGS -Dmprotect=mtrace_noop_mprotect \
  -Dpandecode_find_mapped_gpu_mem_containing=pandecode_find_mapped_gpu_mem_containing_real \
  -c $G/decode_common.c -o $O/decode_common.o
eval clang-19 $FLAGS -Wno-error -fvisibility=default -U_FILE_OFFSET_BITS -c $H/mtrace.c -o $O/mtrace.o
L=mesa-glue/src
eval clang-19 $FLAGS -shared -fuse-ld=lld -o $B/mtrace.so $O/mtrace.o $O/decode_v7.o \
  $O/decode_jm_v7.o $O/decode_common.o \
  $L/panfrost/compiler/bifrost/libpanfrost_bifrost.a.p/*bi_print_common.c.o \
  $L/panfrost/compiler/bifrost/libpanfrost_bifrost_disasm.a \
  $L/panfrost/lib/libpan-format-v7.a $L/panfrost/model/libpanfrost_model_lib.a \
  $L/util/libmesa_util.a $L/util/libmesa_util_simd.a $L/c11/libmesa_util_c11.a \
  $L/util/blake3/libblake3.a -ldl -lpthread -lm
llvm-nm-19 -D --undefined-only $B/mtrace.so | grep -v -E " (GLIBC|_ITM|__cxa|__gmon)" | head -40
ls -la $B/mtrace.so
