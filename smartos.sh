#!/bin/bash
# vim: set ts=8 sts=8 sw=8 noet:

ROOT=$(cd $(dirname $0)/; pwd)

SMARTOS_DIR="${ROOT}/platform"

KERNEL="${SMARTOS_DIR}/i86pc/kernel/amd64/unix"
INITRD="${SMARTOS_DIR}/i86pc/amd64/boot_archive"

CMDLINE_OPTS=(
#	'prom_debug=true'
#	'kbm_debug=true'
#	'map_debug=true'

	'console=ttya'
	'ttya-mode="115200,8,n,1,-"'

#	'smartos=true'
	'standalone=true'
	'noimport=true'

	'root_shadow="$5$tObkceN.$zW4yqc8gkJqDGlxdr41w1cICR9Kfl6KNHmuldLVe8q8"'
)

if (( ${#CMDLINE_OPTS[@]} > 0 )); then
	CMDLINE='-B '
	i=0
	while (( i < ${#CMDLINE_OPTS[@]} )); do
		if (( i > 0 )); then
			CMDLINE="$CMDLINE,"
		fi
		CMDLINE="$CMDLINE${CMDLINE_OPTS[$i]}"
		(( i++ ))
	done
else
	CMDLINE=''
fi

#CMDLINE="$CMDLINE -v -kd"
CMDLINE="$CMDLINE -s"

MEM="-m 1G"
#SMP="-c 2"
#NET="-s 2:0,virtio-net"
#IMG_CD="-s 3,ahci-cd,/somepath/somefile.iso"
#IMG_HDD="-s 4,virtio-blk,/somepath/somefile.img"
PCI_DEV="-s 0:0,hostbridge -s 31,lpc"
LPC_DEV="-l com1,stdio"
UUID="-U deadbeef-dead-dead-dead-deaddeafbeef"

DEBUG_SMARTOS=1 \
build/xhyve $MEM $SMP $PCI_DEV $LPC_DEV $NET $IMG_CD $IMG_HDD $UUID \
  -A \
  -e \
  -f "smartos,$KERNEL,$INITRD,$CMDLINE"

stty sane

