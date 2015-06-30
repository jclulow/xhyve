#!/bin/bash

ROOT=$(cd $(dirname $0)/; pwd)

SMARTOS_DIR="${ROOT}/platform"

KERNEL="${SMARTOS_DIR}/i86pc/kernel/amd64/unix"
INITRD="${SMARTOS_DIR}/i86pc/amd64/boot_archive"
CMDLINE="-B prom_debug=true,map_debug=true,console=ttya,ttya-mode=\"9600,8,n,1,-\" -v -kd"

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
  -g 7890 \
  -e \
  -f "smartos,$KERNEL,$INITRD,$CMDLINE"

stty sane

