#!/bin/sh
# Publish a verified goose.exe and the stdlib to dist/, the one place other
# projects take the compiler from: tools/publish.sh [goose.exe]
# (default build/dev/Release/goose.exe). Run it after each verified commit.
#
# Each publish goes into its own folder, dist/v/<sha>, and dist/goose.exe and
# dist/stdlib are then swapped to it by renames, so a build that is running
# keeps the files it started with and a new one never sees half-written ones.
# Windows lets a running exe be renamed but not overwritten.
set -e
cd "$(dirname "$0")/.." || exit 1
exe=${1:-build/dev/Release/goose.exe}
sha=$(git rev-parse --short=8 HEAD)
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "publish: the tree has uncommitted changes; commit them first" >&2
    exit 1
fi
[ -f "$exe" ] || { echo "publish: no $exe" >&2; exit 1; }
v=dist/v/$sha
rm -rf "${v:?}.tmp"
mkdir -p "$v.tmp"
cp "$exe" "$v.tmp/goose.exe"
cp -r stdlib "$v.tmp/stdlib"
echo "$sha $(git log -1 --format=%cI)" > "$v.tmp/VERSION"
rm -rf "${v:?}"
mv "$v.tmp" "$v"
# The exe: copy beside it under a temporary name, move the old one aside,
# then rename the new one in.
cp "$v/goose.exe" dist/goose.new.exe
[ -f dist/goose.exe ] && mv dist/goose.exe "dist/goose.old-$(date +%s).exe"
mv dist/goose.new.exe dist/goose.exe
# The stdlib: the same, a whole folder at a time.
rm -rf dist/stdlib.new
cp -r "$v/stdlib" dist/stdlib.new
[ -d dist/stdlib ] && mv dist/stdlib "dist/stdlib.old-$(date +%s)"
mv dist/stdlib.new dist/stdlib
cp "$v/VERSION" dist/VERSION
# Old copies a running build may still hold fail to delete; the next run
# retries. Keep the last five published versions.
for f in dist/goose.old-*.exe; do [ -e "$f" ] && rm -f "${f:?}" 2>/dev/null; done
for d in dist/stdlib.old-*; do [ -e "$d" ] && rm -rf "${d:?}" 2>/dev/null; done
ls -dt dist/v/*/ | tail -n +6 | while read -r d; do rm -rf "${d:?}" 2>/dev/null; done
echo "published $sha to $(pwd -W 2>/dev/null || pwd)/dist/goose.exe"
