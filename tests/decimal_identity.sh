#!/bin/sh

_decimal_normalize() {
    case "$1" in ''|*[!0-9]*) return 1 ;; esac
    _decimal_normalized=$1
    while [ "${_decimal_normalized#0}" != "$_decimal_normalized" ]; do
        _decimal_normalized=${_decimal_normalized#0}
    done
    [ -n "$_decimal_normalized" ] || _decimal_normalized=0
}

_decimal_le() {
    _decimal_left=$1
    _decimal_right=$2
    if [ "${#_decimal_left}" -lt "${#_decimal_right}" ]; then return 0; fi
    if [ "${#_decimal_left}" -gt "${#_decimal_right}" ]; then return 1; fi
    [ "$_decimal_left" = "$_decimal_right" ] || [ "$_decimal_left" \< "$_decimal_right" ]
}

decimal_positive_in_range() {
    _decimal_normalize "$1" || return 1
    _decimal_value=$_decimal_normalized
    [ "$_decimal_value" != 0 ] || return 1
    _decimal_normalize "$2" || return 1
    _decimal_min=$_decimal_normalized
    _decimal_normalize "$3" || return 1
    _decimal_max=$_decimal_normalized
    _decimal_le "$_decimal_min" "$_decimal_value" || return 1
    _decimal_le "$_decimal_value" "$_decimal_max"
}
