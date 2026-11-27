#!/bin/sh
# Every register write names its bits (hal/g030_fields.h): a write into a register of ST's structs
# (PERIPH->REG = ..., |=, &=) carries no bare number, except 0 (all bits off) and a whole number in
# a one-number register (a period, a prescaler, a reload, a compare value, a count, a baud divisor),
# and a value placed in a named field (v << NAME_Pos) or given to a named field macro (NAME(n, v)).
# Prints each write it refuses and exits 1 if there is any. Run by tests/run.sh.
here=$(cd "$(dirname "$0")/.." && pwd)
whole='ARR|PSC|RCR|CCR[1-4]|BRR|CNDTR|RLR|PR|LOAD|VAL'
grep -n -E -- '->[A-Z][A-Z0-9]*(\[[0-9]+\])? *[|&]?= ' "$here"/hal/*.c "$here"/bringup/*.c |
  grep -v -E -- "->($whole) *[|&]?= " |
  sed -E -e 's/\[[0-9]+\]//g' -e 's/\(\(uint32_t\) \([^()]*\) << [A-Z][A-Z0-9_]*_Pos\)/FIELD/g' \
    -e 's/([^A-Za-z0-9_])[A-Z][A-Z0-9_]*\([^()]*\)/\1NAMED/g' |
  grep -E -- '[|&]?= [^;]*(\b0x[0-9A-Fa-f]+|\b[1-9][0-9]*)u?\b' |
  grep -v -E -- '//|/\*' > "${TMPDIR:-/tmp}/named_bits.$$"
if [ -s "${TMPDIR:-/tmp}/named_bits.$$" ]; then
  echo "check_named_bits: a register write with an unnamed number (name it in g030_fields.h):"
  cat "${TMPDIR:-/tmp}/named_bits.$$"
  rm -f "${TMPDIR:-/tmp}/named_bits.$$"
  exit 1
fi
rm -f "${TMPDIR:-/tmp}/named_bits.$$"
echo "check_named_bits: every register write names its bits"
