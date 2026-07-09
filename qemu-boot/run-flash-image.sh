#!/bin/sh

if [ $# -ne 1 ]; then
	echo "Usage: $0 <flash_image>"
fi

flash_image=$1

case $flash_image in
	*hi3516ev200*)
		soc=hi3516ev200
		;;
	*hi3516ev300*)
		soc=hi3516ev300
		;;
	*hi3518ev300*)
		soc=hi3518ev300
		;;
	*)
		echo "Unknow SoC Type!"
		exit 1
		;;
esac

# TODO: add non-interactive test cases
echo "### Running qemu-system-arm -M $soc ... ###"
qemu-system-arm -M $soc,flash-file=$flash_image \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no -net nic,netdev=net0 \
    -nographic
