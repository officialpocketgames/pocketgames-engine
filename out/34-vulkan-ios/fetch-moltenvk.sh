#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part B1 (clarification 1, item 1): fetches one repository of moltenvk-sources.txt
# (MoltenVK unless <name> says otherwise) at the commit pinned there into <dest> (a git checkout),
# checks `git rev-parse HEAD` against it, and writes <dest>/.ios-vk-commit. A <dest> that already
# holds that commit is kept. Nothing comes from a prebuilt release. MoltenVK's other dependencies
# are fetched by build-moltenvk.sh, through MoltenVK's own fetchDependencies, at the revisions of
# that commit's ExternalRevisions/.
#
#   fetch-moltenvk.sh <dest> [<name>]
#   fetch-moltenvk.sh --print [<name>]    the name, URL, commit and branch of the pin, nothing fetched

set -eu
here=$(cd "$(dirname "$0")" && pwd)
pins="$here/moltenvk-sources.txt"
[ $# -eq 1 ] || [ $# -eq 2 ] || { echo "usage: $0 <dest> [<name>] | --print [<name>]" >&2; exit 2; }
name=${2:-MoltenVK}
line=$(awk -v n="$name" '$1 == n { print; exit }' "$pins")
[ -n "$line" ] || { echo "fetch-moltenvk: no line for $name in $pins" >&2; exit 1; }
url=$(echo "$line" | awk '{ print $2 }')
commit=$(echo "$line" | awk '{ print $3 }')
tag=$(echo "$line" | awk '{ print $4 }')
echo "$commit" | grep -qxE '[0-9a-f]{40}' || { echo "fetch-moltenvk: no 40-hex commit for $name in $pins" >&2; exit 1; }
if [ "$1" = --print ]; then
    echo "$name $url $commit $tag"
    exit 0
fi
dest=$1

# refs/ is read only: refuse a destination that is, or would be created, inside it (checked on the
# nearest existing parent, links resolved, before anything is created)
refs=$(cd "$here/../../refs" 2>/dev/null && pwd -P) || refs=""
if [ -n "$refs" ]; then
    p=$dest; rest=""
    while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
    case "$(cd "$p" && pwd -P)$rest/" in
        "$refs/"*) echo "fetch-moltenvk: refusing a destination inside refs/: $dest" >&2; exit 1 ;;
    esac
fi
mkdir -p "$dest"

if [ -f "$dest/.ios-vk-commit" ] && [ "$(cat "$dest/.ios-vk-commit")" = "$commit" ] &&
   [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
    echo "fetch-moltenvk: $dest already holds $name $tag ($commit)"
    exit 0
fi
rm -rf "$dest"
mkdir -p "$dest"
git -C "$dest" init -q
if git -C "$dest" fetch -q --depth 1 "$url" "$commit"; then
    git -C "$dest" checkout -q --detach FETCH_HEAD
else
    # a server that refuses a fetch by hash: clone the branch and check the commit out
    echo "fetch-moltenvk: shallow fetch of $commit refused, cloning $url ($tag)"
    rm -rf "$dest"
    if [ "$tag" = - ] || ! git clone -q --branch "$tag" "$url" "$dest"; then
        rm -rf "$dest"
        git clone -q "$url" "$dest"
    fi
    git -C "$dest" checkout -q --detach "$commit"
fi
head=$(git -C "$dest" rev-parse HEAD)
[ "$head" = "$commit" ] || { echo "fetch-moltenvk: HEAD is $head, moltenvk-sources.txt pins $commit" >&2; exit 1; }
echo "$commit" > "$dest/.ios-vk-commit"
echo "fetch-moltenvk: $name $tag at $commit (git rev-parse HEAD verified)"
