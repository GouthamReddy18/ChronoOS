#!/bin/sh
set -eu
. tests/decimal_identity.sh

decimal_positive_in_range 2 2 2147483647
decimal_positive_in_range 2147483647 2 2147483647
decimal_positive_in_range 00042 2 2147483647
decimal_positive_in_range 18446744073709551615 1 18446744073709551615

for invalid in '' 0 1 -1 +2 2147483648 999999999999999999999999999999999; do
    if decimal_positive_in_range "$invalid" 2 2147483647; then
        echo "test_launch_identity: accepted invalid PID '$invalid'" >&2
        exit 1
    fi
done
for invalid in '' 0 -1 +1 18446744073709551616 999999999999999999999999999999999; do
    if decimal_positive_in_range "$invalid" 1 18446744073709551615; then
        echo "test_launch_identity: accepted invalid STARTTIME '$invalid'" >&2
        exit 1
    fi
done
echo "launch identity validation tests passed"
