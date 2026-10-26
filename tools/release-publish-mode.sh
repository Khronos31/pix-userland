#!/bin/sh
set -eu
candidate=${1:?Usage: release-publish-mode.sh CANDIDATE GITHUB_REF}
ref=${2:?Usage: release-publish-mode.sh CANDIDATE GITHUB_REF}
case "$candidate" in
    true) echo false;;
    false)
        [ "$ref" = refs/heads/main ] || {
            echo 'Publishing is allowed only from main' >&2
            exit 1
        }
        echo true
        ;;
    *) echo 'Candidate input must be true or false' >&2; exit 2;;
esac
