#!/bin/sh

#
# скрипт для быстрой сборки nginx-full из backports
#

set -e

SRC_URL="http://mirror.yandex.ru/debian/pool/main/n/nginx/nginx_1.6.2-5%2ba.exp1.dsc"

WORK_DIR=$(basename "${SRC_URL}" | cut -d '~' -f 1 | cut -d '-' -f 1 | sed -e 's/_/-/')

dget -u "${SRC_URL}"

cd "${WORK_DIR}"

git clone git://github.com/abbat/nginx-sla.git debian/modules/nginx-sla

patch -p0 < ../debian.patch

dpkg-buildpackage -rfakeroot -D -us -uc -b

cd ..

rm -rf "${WORK_DIR}"
rm -f *.dsc
rm -f *.changes
rm -f *.tar.gz

rm -f nginx_*_all.deb
rm -f nginx-common_*_all.deb
rm -f nginx-doc_*_all.deb
rm -f nginx-extras_*_*.deb
rm -f nginx-extras-dbg_*_*.deb
rm -f nginx-light_*_*.deb
rm -f nginx-light-dbg_*_*.deb
rm -f nginx-naxsi_*_*.deb
rm -f nginx-naxsi-dbg_*_*.deb
rm -f nginx-naxsi-ui_*_all.deb
