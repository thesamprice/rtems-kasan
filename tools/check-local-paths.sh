#!/bin/sh
#
# Fail if any tracked file contains an absolute path from a developer machine.
#
# This repository publishes build logs, dejagnu .sum files and objdump output,
# all of which embed srcdir/builddir paths.  Three separate commits leaked a
# home directory layout before this check existed.
#
# Usage:
#   tools/check-local-paths.sh            # check tracked files
#   tools/check-local-paths.sh --staged   # check what is about to be committed
#
# Scrub with placeholders rather than deleting the files:
#   <srcdir> <builddir> <scratch> <home> <toolchain>

set -e

if [ "$1" = "--staged" ]; then
    files=$(git diff --cached --name-only --diff-filter=ACM)
else
    files=$(git ls-files)
fi

[ -z "$files" ] && exit 0

# /Users/name/ and /home/name/ are developer home directories.
# /tmp/claude-NNN and /private/tmp/claude-NNN are agent scratch directories.
pattern='(/private)?/tmp/claude-[0-9]+|/Users/[A-Za-z0-9_.-]+/|/home/[A-Za-z0-9_.-]+/'

hits=0
for f in $files; do
    [ -f "$f" ] || continue
    case "$f" in
        tools/check-local-paths.sh|.github/workflows/*) continue ;;
    esac
    if grep -InE "$pattern" "$f" >/dev/null 2>&1; then
        if [ "$hits" -eq 0 ]; then
            echo "ERROR: local absolute paths found in tracked files:" >&2
            echo >&2
        fi
        hits=1
        grep -InE "$pattern" "$f" 2>/dev/null | head -3 | sed "s|^|  $f:|" >&2
        n=$(grep -IcE "$pattern" "$f" 2>/dev/null || echo 0)
        [ "$n" -gt 3 ] && echo "  $f: ... and $((n - 3)) more line(s)" >&2
    fi
done

if [ "$hits" -ne 0 ]; then
    cat >&2 <<'MSG'

Replace them with placeholders before committing, for example:

  sed -i '' -e "s|$PWD|<builddir>|g" -e "s|$HOME|<home>|g" offending-file

MSG
    exit 1
fi

exit 0
