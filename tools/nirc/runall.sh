#!/bin/bash
# runall.sh OUT [VAR=value...] - nirc over every dump in $DUMPS, $JOBS at a time (default 16), each
# under a $TMO-second timeout (default 600), with the given environment (compiler knobs). $NIRC is
# the binary (default <tree>/build-host/nirc). Per shader: OUT/<name>.out (nirc's line) and
# OUT/<name>.err (the compiler's stderr); OUT/summary.txt gets the lines sorted by compile time,
# timeouts marked.
H=$(dirname "$(readlink -f "$0")")
O=$1; shift
D=${DUMPS:?set DUMPS to a directory of .mxnir dumps (MALISX2_NIR_DUMP)}
N=${NIRC:-$(readlink -f "$H/../..")/build-host/nirc}
mkdir -p "$O"
export N O TMO=${TMO:-600}
ls "$D"/*.mxnir | env "$@" xargs -P "${JOBS:-16}" -I{} bash -c \
  'f={}; b=$(basename $f .mxnir); timeout $TMO $N $f > $O/$b.out 2> $O/$b.err || echo "$b TIMEOUT/FAIL rc=$?" > $O/$b.out'
cat "$O"/*.out | awk '{for (i = 1; i <= NF; i++) if ($(i+1) == "ms") { print $i, $0; next } print 999999999, $0}' \
  | sort -n | cut -d" " -f2- > "$O/summary.txt"
grep -c . "$O/summary.txt"
