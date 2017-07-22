#!/bin/bash

[[ -f Makefile ]] && ./utils/clean.sh

DIRS=(
	test/entrance/themes
)

for d in ${DIRS[@]}; do
	[[ ! -d "${d}" ]] && mkdir -p "${d}"
done

MY_PWD="$(pwd)"

./autogen.sh \
	--prefix "${MY_PWD}" \
	--libdir "${MY_PWD}/test" \
	--datarootdir "${MY_PWD}/test" \
	--sysconfdir "${MY_PWD}/data"

make

cp src/bin/entrance_client \
	src/daemon/entrance_ck_launch \
	src/daemon/entrance_wait \
	test/entrance

cp data/themes/default/default.edj test/entrance/themes
chmod 664 test/entrance/themes/default.edj
