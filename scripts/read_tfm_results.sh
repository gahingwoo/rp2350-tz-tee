#!/usr/bin/env bash
#
# read_tfm_results.sh -- verdict on the TF-M regression run over pure SWD.
#
# No UART needed. The TF-M test framework keeps a table of `struct test_suite_t`
# in SRAM; after the run each suite's result field holds 0 (== TEST_PASSED).
# We halt the core over CMSIS-DAP (RPi Debug Probe), reset-init so SRAM keeps
# the last run's contents, read the two suite arrays, and count the val fields.
#
# Verified layout on rpi/rp2350, profile_medium, tf-m-tests 2.x (all little-endian):
#
#   struct test_suite_t (5 words / 20 bytes):
#     +0x00  freg        test-registration fn ptr   (flash, 0x10xxxxxx)
#     +0x04  test_list   ptr to the per-suite tests (SRAM, 0x20xxxxxx)
#     +0x08  list_size   number of tests in suite   (non-zero == populated)
#     +0x0c  name        suite name string ptr      (flash, 0x10xxxxxx)
#     +0x10  val         suite result: 0 == PASSED  (<-- the field we judge)
#
#   Non-Secure suite array base: 0x2004001c  (7 suites)
#   Secure     suite array base: 0x20015008  (9 suites)
#   => 16 suites total. A suite is a PASS iff val==0 AND list_size!=0.
#
# The array end is a zeroed / non-flash freg word, so we walk structs until
# freg no longer looks like a flash pointer (0x10......).
#
# Everything below is overridable from the environment; defaults match the
# reference bring-up tree under ~/rp2350-tz.
set -euo pipefail

ROOT="${ROOT:-$HOME/rp2350-tz}"
OCD_BIN="${OCD_BIN:-$ROOT/openocd-rpi/src/openocd}"
OCD_SCRIPTS="${OCD_SCRIPTS:-$ROOT/openocd-rpi/tcl}"
OCD_IFACE="${OCD_IFACE:-interface/cmsis-dap.cfg}"
OCD_TARGET="${OCD_TARGET:-target/rp2350.cfg}"
ADAPTER_KHZ="${ADAPTER_KHZ:-2000}"

NS_ADDR="${NS_ADDR:-0x2004001c}"
S_ADDR="${S_ADDR:-0x20015008}"
READ_WORDS="${READ_WORDS:-60}"   # per array; 60 words = up to 12 suites, plenty

if [[ ! -x "$OCD_BIN" ]]; then
    echo "error: openocd not found/executable at: $OCD_BIN" >&2
    echo "       set OCD_BIN (a build that knows rp2350; the apt 0.12.0 does not)." >&2
    exit 2
fi

raw="$(
    "$OCD_BIN" -s "$OCD_SCRIPTS" \
        -f "$OCD_IFACE" -f "$OCD_TARGET" \
        -c "adapter speed $ADAPTER_KHZ" \
        -c "init" \
        -c "reset init" \
        -c "mdw $NS_ADDR $READ_WORDS" \
        -c "mdw $S_ADDR $READ_WORDS" \
        -c "shutdown" 2>&1
)" || { echo "$raw" >&2; echo "error: openocd invocation failed" >&2; exit 2; }

# Split the two arrays by their SRAM address prefix (NS=0x2004*, S=0x2001*).
ns_lines="$(printf '%s\n' "$raw" | grep -E '^0x2004' || true)"
s_lines="$(printf  '%s\n' "$raw" | grep -E '^0x2001' || true)"

# Walk 5-word structs; stop at the first freg that is not a flash pointer.
# Emits "<total> <passed>" and a per-suite line to stderr for the eyeball test.
judge() {
    local label="$1" lines="$2"
    printf '%s\n' "$lines" \
    | sed -E 's/^0x[0-9a-fA-F]+:\s*//' \
    | awk -v label="$label" '
        { for (i = 1; i <= NF; i++) w[n++] = $i }
        END {
            total = 0; passed = 0;
            for (i = 0; i + 4 < n; i += 5) {
                freg = w[i]; list_size = w[i+2]; val = w[i+4];
                if (substr(freg, 1, 2) != "10") break;   # end of array
                total++;
                ok = (val == "00000000" && list_size != "00000000");
                if (ok) passed++;
                printf("  %-3s suite %-2d  list_size=0x%s  val=0x%s  %s\n",
                       label, total, list_size, val, ok ? "PASS" : "FAIL") > "/dev/stderr";
            }
            printf("%d %d\n", total, passed);
        }'
}

read ns_total ns_pass < <(judge "NS" "$ns_lines")
read s_total  s_pass  < <(judge "S"  "$s_lines")

total=$((ns_total + s_total))
passed=$((ns_pass + s_pass))

echo "--------------------------------------------------"
echo "Non-Secure: $ns_pass/$ns_total passed"
echo "Secure    : $s_pass/$s_total passed"
echo "TOTAL     : $passed/$total passed"
echo "--------------------------------------------------"

if [[ "$total" -gt 0 && "$passed" -eq "$total" ]]; then
    echo "RESULT: ALL PASSED"
    exit 0
else
    echo "RESULT: FAILURE (or no suites found -- did the firmware run before this read?)"
    exit 1
fi
