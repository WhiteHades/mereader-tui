#!/bin/sh

set -eu

binary=${1:?usage: cli_test.sh /path/to/baca}
root=$PWD/build/test-tmp/cli

cleanup() {
    rm -rf "$root"
    rmdir "$PWD/build/test-tmp" 2>/dev/null || :
}

fail() {
    printf 'FAIL cli.%s\n' "$1" >&2
    exit 1
}

pass() {
    printf 'PASS cli.%s\n' "$1"
}

trap cleanup EXIT HUP INT TERM
cleanup
mkdir -p "$root/home" "$root/xdg-config" "$root/xdg-cache" "$root/tmp"
export HOME="$root/home"
export XDG_CONFIG_HOME="$root/xdg-config"
export XDG_CACHE_HOME="$root/xdg-cache"
export TMPDIR="$root/tmp"
export LC_ALL=C

help_output=$("$binary" --help 2>&1) || fail help
case $help_output in
    *"usage: baca"*"TUI ebook reader"*"--history"*) ;;
    *) fail help ;;
esac
pass help

version_output=$("$binary" --version 2>&1) || fail version
[ "$version_output" = "v0.2.0" ] || fail version
pass version

history_output=$("$binary" --history 2>&1) || fail history
case $history_output in
    "Baca History"*"Last Read"*"Progress"*"Path"*) ;;
    *) fail history ;;
esac
[ -f "$XDG_CACHE_HOME/baca/baca.db" ] || fail history
pass history

if missing_output=$("$binary" 2>&1); then
    fail missing_selection
fi
case $missing_output in
    *"baca: no reading history"*) ;;
    *) fail missing_selection ;;
esac
pass missing_selection

if number_output=$("$binary" 1 2>&1); then
    fail missing_history_number
fi
case $number_output in
    *"Baca History"*"history entry #1 not found"*) ;;
    *) fail missing_history_number ;;
esac
pass missing_history_number

if option_output=$("$binary" --definitely-invalid 2>&1); then
    fail unknown_option
fi
case $option_output in
    *"baca: unknown option"*) ;;
    *) fail unknown_option ;;
esac
pass unknown_option

if command -v sqlite3 >/dev/null 2>&1; then
    book="$root/control.epub"
    : > "$book"
    database="$XDG_CACHE_HOME/baca/baca.db"
    sqlite3 "$database" \
        "INSERT OR REPLACE INTO reading_history(filepath,title,author,reading_progress,last_read) VALUES("\
"'$book','Bad'||char(10)||'Title'||char(9)||'Name'||char(27)||'[31m',"\
"'Auth'||char(127)||'or',0.5,'2026-07-18 12:00:00');"
    control_output=$("$binary" --history 2>&1) || fail unsafe_history
    escape=$(printf '\033')
    case $control_output in
        *"$escape"*) fail unsafe_history ;;
    esac
    case $control_output in
        *"Bad Title Name [31m"*"Auth or"*) ;;
        *) fail unsafe_history ;;
    esac
    pass unsafe_history
else
    printf 'SKIP cli.unsafe_history: sqlite3 CLI unavailable\n'
fi

printf 'SUMMARY cli passed\n'
