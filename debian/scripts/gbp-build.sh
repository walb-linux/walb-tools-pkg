#!/bin/sh

COWBUILDER_BASEPATH=/var/cache/pbuilder/bionic/base.cow

gbp buildpackage \
--git-pristine-tar \
--git-pristine-tar-commit \
--git-ignore-branch \
--git-builder="pdebuild --pbuilder cowbuilder --debbuildopts -j12 -- --basepath ${COWBUILDER_BASEPATH}" \
--git-upstream-tag='v%(version)s'

