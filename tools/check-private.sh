#!/bin/sh
set -eu
: "${GH_REPO:?Set GH_REPO}"
[ "$GH_REPO" = 'Khronos31/pix-userland' ] || { echo 'Unexpected repository' >&2; exit 1; }
[ "$(gh api "repos/$GH_REPO" --jq .private)" = true ] || { echo 'PRIVATE repository required; refusing to proceed' >&2; exit 1; }
