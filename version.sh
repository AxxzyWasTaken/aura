#!/bin/sh
cd "$(dirname "$0")"

dirbase="$(basename "$(pwd)")"

if [ -n "$AURA_VERSION_TAG" ]; then
    version="$AURA_VERSION_TAG"
elif [ -n "$M1N1_VERSION_TAG" ]; then
    version="$M1N1_VERSION_TAG"
elif [ -e ".git" ]; then
    version="$(git describe --tags --always --dirty)"
elif [ "$(echo "${dirbase}" | cut -c1-5)" = "aura-" ]; then
    version=$(echo "${dirbase}" | cut -c6-)
    version="v${version##v}"
else
    version="unknown"
fi

echo "#define BUILD_TAG \"$version\""
