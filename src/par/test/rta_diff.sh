#!/bin/bash
# Compare static vs RTA partition output. Usage: rta_diff.sh static.part rta.part
set -e
STATIC=$1
RTA=$2
if [ ! -f "$STATIC" ] || [ ! -f "$RTA" ]; then
  echo "missing partition files: $STATIC / $RTA"; exit 1
fi
n_static=$(wc -l < "$STATIC")
n_rta=$(wc -l < "$RTA")
echo "=== partition file sizes ==="
echo "static: $n_static lines"
echo "rta:    $n_rta lines"
if [ "$n_static" != "$n_rta" ]; then
  echo "WARN: partition files differ in length"
fi
echo
echo "=== block-size distribution (count of vertices per block id) ==="
echo -n "static: "; awk '{print $NF}' "$STATIC" | sort | uniq -c | tr '\n' '|'; echo
echo -n "rta:    "; awk '{print $NF}' "$RTA"    | sort | uniq -c | tr '\n' '|'; echo
echo
echo "=== per-line diff (vertices that changed blocks) ==="
# Partition file format is "<inst_name>  <block_id>" with $NF = block_id.
# Compare block ids only (NOT instance names, which always match line-by-line).
diff_lines=$(paste "$STATIC" "$RTA" | awk '{
  ns = split($0, a, "\t"); if (ns != 2) next;
  m1 = split(a[1], b1, " "); m2 = split(a[2], b2, " ");
  if (b1[m1] != b2[m2]) n++;
} END { print n+0 }')
echo "$diff_lines vertices changed blocks ($n_static total)"
if [ "$n_static" -gt 0 ]; then
  pct=$(awk -v d="$diff_lines" -v n="$n_static" 'BEGIN {printf "%.1f", 100*d/n}')
  echo "($pct% of vertices reassigned)"
fi
