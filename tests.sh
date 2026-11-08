#!/bin/bash
# Automated checks for GateSim. Run with: make test
pass=0; fail=0
check() {  # check "description" command...
    local desc="$1"; shift
    if "$@" > /dev/null 2>&1; then echo "PASS  $desc"; pass=$((pass+1))
    else echo "FAIL  $desc"; fail=$((fail+1)); fi
}
expect_fail() { ! ./gatesim "$@"; }

# Every valid example must run.
for f in examples/*.circ; do check "runs: $f" ./gatesim "$f"; done
# Every broken example must be rejected.
for f in examples/errors/*.circ; do check "rejects: $f" expect_fail "$f"; done

# Exhaustively compare the 4-bit adder with real arithmetic (512 cases).
adder_ok() {
  ./gatesim examples/adder4.circ | tail -n +2 | while read -r a3 a2 a1 a0 b3 b2 b1 b0 c _ co s3 s2 s1 s0; do
    a=$((a3*8+a2*4+a1*2+a0)); b=$((b3*8+b2*4+b1*2+b0))
    got=$((co*16+s3*8+s2*4+s1*2+s0))
    [ $got -eq $((a+b+c)) ] || { echo "mismatch $a+$b+$c=$got"; exit 1; }
  done
}
check "adder4 matches arithmetic for all 512 inputs" adder_ok

# Test vector mode: 1 + 1 + 1 = 11 in binary.
check "vector mode: full adder 1+1+1" bash -c \
  './gatesim examples/full_adder.circ A=1 B=1 CIN=1 | tail -1 | grep -q "1 1   1 |   1    1"'
check "vector mode rejects missing input" expect_fail examples/full_adder.circ A=1 B=1
check "vector mode rejects non-input"     expect_fail examples/full_adder.circ A=1 B=1 CIN=1 T1=0

echo; echo "$pass passed, $fail failed"; [ $fail -eq 0 ]
