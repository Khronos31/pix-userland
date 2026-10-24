#!/bin/sh
set -eu
: "${GH_REPO:?Set GH_REPO}"
[ "$GH_REPO" = 'Khronos31/pix-userland' ] || { echo 'Unexpected repository' >&2; exit 1; }
# Historical script name; the owner approved and completed PUBLIC visibility.
[ "$(gh api "repos/$GH_REPO" --jq .private)" = false ] || { echo 'PUBLIC repository required; refusing to proceed' >&2; exit 1; }
