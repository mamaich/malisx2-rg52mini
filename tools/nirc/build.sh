#!/bin/bash
# build.sh - nirc (nirc.c) against a native x86-64 build of this tree's compiler.
# HOST_BUILD (default <tree>/build-host) is a `meson setup -Dbuildtype=debugoptimized` of the tree
# for the host (asserts on); its mesa-glue libraries are rebuilt first, so a change in mesa/ is
# picked up. Output: $HOST_BUILD/nirc (or $NIRC_OUT there).
set -e
H=$(dirname "$(readlink -f "$0")")
S=$(readlink -f "$H/../..")
B=${HOST_BUILD:-$S/build-host}
cd "$B"
T=$(ninja -t targets all | sed -n 's#^\(mesa-glue/[^:]*\.a\): .*#\1#p' | grep -v vulkan | sort)
ninja $T > nirc-ninja.log 2>&1 || { grep -m10 -B2 -A8 -i error nirc-ninja.log; exit 1; }
I="-Imesa-glue/src -I$S/mesa-glue/src -I$S/mesa/include -I$S/mesa/src -Imesa-glue/src/panfrost
   -I$S/mesa-glue/src/panfrost -Imesa-glue/src/panfrost/compiler -I$S/mesa/src/panfrost
   -I$S/mesa/src/panfrost/compiler -Imesa-glue/src/compiler/nir -I$S/mesa/src/compiler/nir
   -Imesa-glue/src/compiler -I$S/mesa/src/compiler -Imesa-glue/src/util -Imesa-glue/src/util/format
   -I$S/mesa/src/panfrost/compiler/bifrost -Imesa-glue/src/panfrost/compiler/bifrost"
D="-D_GNU_SOURCE -DMESA_DEBUG=0 -DHAVE_PTHREAD -DHAVE_ENDIAN_H -DUSE_GCC_ATOMIC_BUILTINS
   -DHAVE_FUNC_ATTRIBUTE_PACKED -DHAVE_FUNC_ATTRIBUTE_UNUSED -DHAVE___BUILTIN_EXPECT
   -DHAVE___BUILTIN_CLZ -DHAVE___BUILTIN_CLZLL -DHAVE___BUILTIN_CTZ -DHAVE___BUILTIN_FFS
   -DHAVE___BUILTIN_FFSLL -DHAVE___BUILTIN_POPCOUNT -DHAVE___BUILTIN_POPCOUNTLL
   -DHAVE___BUILTIN_UNREACHABLE -DHAVE_STRUCT_TIMESPEC"
cc -std=c11 -O2 -g $D $I -c "$H/nirc.c" -o nirc.o
cc -o ${NIRC_OUT:-nirc} nirc.o -Wl,--start-group $T -Wl,--end-group -lm -lpthread -ldl -lzstd -lz
echo "nirc: $B/${NIRC_OUT:-nirc}"
