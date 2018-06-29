#!/bin/sh

DEBIAN_VER=$1

if [ "$DEBIAN_VER" = "" ]; then
    echo "Specify debian version like '1.0.0~beta1-0xenial1'."
    exit 1
fi

gbp dch -N ${DEBIAN_VER} --ignore-branch --commit
gbp buildpackage --git-tag-only --git-ignore-branch
sh debian/scripts/gbp-build.sh
