#!/bin/sh
# Prints the BUILD_TAG define: the release in VERSION, plus the commit when the
# tree is not exactly that release's tag. AURA_VERSION_TAG overrides it all.
cd "$(dirname "$0")"

release="$(cat VERSION 2>/dev/null)"
release="${release:-unknown}"
dirbase="$(basename "$(pwd)")"

if [ -n "$AURA_VERSION_TAG" ]; then
    version="${AURA_VERSION_TAG#v}"
elif [ -e ".git" ] && commit="$(git rev-parse --short=8 HEAD 2>/dev/null)"; then
    if [ "$(git describe --tags --exact-match HEAD 2>/dev/null)" = "v$release" ]; then
        version="$release"
    else
        version="$release-g$commit"
    fi
    git diff --quiet HEAD -- 2>/dev/null || version="$version-dirty"
elif [ "$(echo "${dirbase}" | cut -c1-5)" = "aura-" ]; then
    commit="$(echo "${dirbase}" | cut -c6-13)"
    version="$release-g$commit"
else
    version="$release"
fi

echo "#define BUILD_TAG \"$version\""
