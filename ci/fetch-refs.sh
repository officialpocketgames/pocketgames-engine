#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# CI: fetch the reference sources that refs/.gitignore keeps out of git, as refs/README.md lists
# them, and unpack each into refs/<name>/.
#
#   ci/fetch-refs.sh <download dir> [<name>...]      (default: every folder of refs/.gitignore)
#
# For each name, the row of refs/README.md whose first cell is `<name>/` gives the URL (its
# second cell, https:// added) and the tarball's SHA-256 ("SHA-256 `<hex>`"). The tarball is
# downloaded into <download dir> unless it is already there with that hash, and refused on a
# mismatch. Where the row gives no SHA-256 (wine-11.4), ci/refs-trees.sha256 must give the
# digest of the unpacked tree instead (see tree_digest below; it was computed from the owner's
# own copy of refs/<name>), and the tarball's SHA-256 is printed so it can be added to the README.
# A refs/<name>/ that already exists is not unpacked again, but a tree digest is still checked.
# A git row (a repository URL and "commit `<40 hex>`", e.g. BearSSL) is fetched at that commit
# and checked by its hash (fetch_git below). Names listed in ci/refs-skip.txt are not fetched.
# FETCH_REFS_DRY=1 only prints what each name resolves to; FETCH_REFS_TREE_ONLY=1 only checks the
# tree digests of the refs/<name> already there (ci/check-ci.sh uses both).

set -eu

[ $# -ge 1 ] || { echo "usage: $0 <download dir> [<name>...]" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
mkdir -p "$1"
dl=$(cd "$1" && pwd)
shift
readme="$repo/refs/README.md"
trees="$here/refs-trees.sha256"

if [ $# -eq 0 ]; then
    # shellcheck disable=SC2046
    set -- $(sed -n 's|^\([A-Za-z0-9._+-]*\)/$|\1|p' "$repo/refs/.gitignore")
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}

# tree_digest <dir> <name>: SHA-256 of the sorted list of "<sha256> *./<path>" lines of the
# tree's regular files (sha256sum -b format; the same on Linux, macOS and Git Bash), leaving out
# the paths ci/refs-trees.exclude lists for <name> (files of the owner's copy that are not
# upstream's, so the digest covers only what the copy and the tarball should share).
tree_digest() {
    excl=$(mktemp)
    awk -v n="$2" '$1 == n {print $2}' "$here/refs-trees.exclude" 2>/dev/null > "$excl" || true
    (
        cd "$1"
        if command -v sha256sum >/dev/null 2>&1; then sum="sha256sum -b"; else sum="shasum -a 256 -b"; fi
        # shellcheck disable=SC2086
        find . -type f | LC_ALL=C sort | grep -vxF -f "$excl" | tr '\n' '\0' | xargs -0 $sum | $sum | awk '{print $1}'
    )
    rm -f "$excl"
}

# fetch_git <name> <url> <commit> <row>: a git row. The commit is fetched alone (a shallow
# fetch by hash; a full clone where the server refuses that), `git rev-parse HEAD` must equal it,
# and it is kept in <download dir> as `git archive --prefix=<name>/ <commit> | gzip -n`, which
# carries the commit in its pax header (git get-tar-commit-id): a cached archive is checked
# against the commit the same way. Unpacked into refs/<name>/ unless that exists. The README's
# SHA-256 of its own export is compared for information only (the commit is the identity).
fetch_git() {
    file="$dl/$1.git-$3.tar.gz"
    if [ -f "$file" ] && [ "$(gzip -dc "$file" | git get-tar-commit-id 2>/dev/null || true)" != "$3" ]; then
        echo "fetch-refs: $1: cached $(basename "$file") is not commit $3; fetching again"
        rm -f "$file"
    fi
    if [ ! -f "$file" ]; then
        g=$(mktemp -d)
        git -C "$g" init -q
        if git -C "$g" fetch -q --depth 1 "$2" "$3" 2>"$g/.err"; then
            git -C "$g" checkout -q --detach FETCH_HEAD
        else
            echo "fetch-refs: $1: shallow fetch of $3 refused ($(head -n 1 "$g/.err")); cloning $2"
            rm -rf "$g"
            g=$(mktemp -d)
            if ! git clone -q "$2" "$g" || ! git -C "$g" checkout -q --detach "$3"; then
                echo "::error::fetch-refs: $1: could not clone $2 at $3"
                rm -rf "$g"; return 1
            fi
        fi
        head=$(git -C "$g" rev-parse HEAD)
        if [ "$head" != "$3" ]; then
            echo "::error::fetch-refs: $1: HEAD is $head, refs/README.md pins $3"
            rm -rf "$g"; return 1
        fi
        git -C "$g" archive --format=tar --prefix="$1/" "$3" | gzip -n > "$file.part"
        rm -rf "$g"
        mv "$file.part" "$file"
    fi
    echo "fetch-refs: $1: commit $3 verified (git rev-parse HEAD; cached as $(basename "$file"))"
    readme_sha=$(printf '%s\n' "$4" | sed -n 's/.*SHA-256 `\([0-9a-f]\{64\}\)`.*/\1/p')
    if [ -n "$readme_sha" ]; then
        if [ "$(sha256_of "$file")" = "$readme_sha" ]; then
            echo "fetch-refs: $1: the export's SHA-256 also matches refs/README.md"
        else
            echo "fetch-refs: $1: the export's SHA-256 differs from refs/README.md's (gzip/git versions; the commit is what counts)"
        fi
    fi
    dest="$repo/refs/$1"
    if [ ! -d "$dest" ]; then
        rm -rf "$dest.new"
        mkdir -p "$dest.new"
        tar -xzf "$file" -C "$dest.new" --strip-components=1 || { rm -rf "$dest.new"; return 1; }
        mv "$dest.new" "$dest"
        echo "fetch-refs: unpacked into refs/$1"
    fi
}

status=0
for name in "$@"; do
    row=$(grep -F "| \`$name/\` |" "$readme" | head -n 1 || true)
    if [ -z "$row" ]; then
        echo "::error::fetch-refs: no row for $name/ in refs/README.md"
        status=1; continue
    fi
    url=$(printf '%s\n' "$row" | awk -F'|' '{print $3}' | sed -n 's/^[^`]*`\([^`]*\)`.*/\1/p')
    case "$url" in
        https://*|http://*) ;;
        *) url="https://$url" ;;
    esac
    skip=$(awk -v n="$name" '$1 == n { $1 = ""; sub(/^ +/, ""); print; exit }' "$here/refs-skip.txt" 2>/dev/null || true)
    if [ -n "$skip" ]; then
        echo "fetch-refs: $name: skipped ($skip)"
        continue
    fi
    # a git row: a repository URL (not an archive) and the commit to check out
    commit=$(printf '%s\n' "$row" | awk -F'|' '{print $3}' | sed -n 's/.*commit `\([0-9a-f]\{40\}\)`.*/\1/p')
    case "$url" in *.tar.*|*.tgz|*.zip) commit="" ;; esac
    if [ -n "$commit" ]; then
        if [ "${FETCH_REFS_DRY:-0}" = 1 ]; then
            echo "dry: $name <- $url commit=$commit (git)"
        elif [ "${FETCH_REFS_TREE_ONLY:-0}" != 1 ]; then
            fetch_git "$name" "$url" "$commit" "$row" || status=1
        fi
        continue
    fi
    want=$(printf '%s\n' "$row" | sed -n 's/.*SHA-256 `\([0-9a-f]\{64\}\)`.*/\1/p')
    want_tree=$(awk -v n="$name" '$2 == n {print $1}' "$trees" 2>/dev/null || true)
    if [ -z "$want" ] && [ -z "$want_tree" ]; then
        echo "::error::fetch-refs: $name has no SHA-256 in refs/README.md and no tree digest in ci/refs-trees.sha256"
        status=1; continue
    fi
    if [ "${FETCH_REFS_DRY:-0}" = 1 ]; then
        echo "dry: $name <- $url sha256=${want:-none} tree=${want_tree:-none}"
        continue
    fi
    if [ "${FETCH_REFS_TREE_ONLY:-0}" = 1 ]; then
        [ -n "$want_tree" ] && [ -d "$repo/refs/$name" ] || continue
        got_tree=$(tree_digest "$repo/refs/$name" "$name")
        echo "tree: $name $got_tree (ci/refs-trees.sha256: $want_tree)"
        [ "$got_tree" = "$want_tree" ] || status=1
        continue
    fi
    file="$dl/$(basename "$url")"
    if [ -f "$file" ] && [ -n "$want" ] && [ "$(sha256_of "$file")" != "$want" ]; then
        rm -f "$file"
    fi
    if [ ! -f "$file" ]; then
        echo "fetch-refs: downloading $url"
        if ! curl -fL --retry 4 --retry-delay 10 -o "$file.part" "$url"; then
            echo "::error::fetch-refs: download failed: $url"
            rm -f "$file.part"; status=1; continue
        fi
        mv "$file.part" "$file"
    fi
    got=$(sha256_of "$file")
    if [ -n "$want" ]; then
        if [ "$got" != "$want" ]; then
            echo "::error::fetch-refs: SHA-256 mismatch for $(basename "$file"): got $got, refs/README.md says $want"
            rm -f "$file"; status=1; continue
        fi
        echo "fetch-refs: $name: tarball SHA-256 $got matches refs/README.md"
    else
        echo "::notice::fetch-refs: $name: refs/README.md gives no SHA-256; the tarball's is $got (checked through the tree digest below)"
    fi
    dest="$repo/refs/$name"
    if [ ! -d "$dest" ]; then
        rm -rf "$dest.new"
        mkdir -p "$dest.new"
        if ! tar -xf "$file" -C "$dest.new" --strip-components=1; then
            echo "::error::fetch-refs: could not unpack $(basename "$file")"
            rm -rf "$dest.new"; status=1; continue
        fi
        mv "$dest.new" "$dest"
        echo "fetch-refs: unpacked into refs/$name"
    fi
    if [ -n "$want_tree" ]; then
        got_tree=$(tree_digest "$dest" "$name")
        if [ "$got_tree" != "$want_tree" ]; then
            echo "::error::fetch-refs: refs/$name differs from the owner's copy (tree digest $got_tree, ci/refs-trees.sha256 says $want_tree)"
            status=1; continue
        fi
        echo "fetch-refs: $name: tree digest $got_tree matches ci/refs-trees.sha256"
    fi
done
exit $status
