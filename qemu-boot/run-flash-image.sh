#!/bin/sh -e

SCRIPT_DIR="$(dirname "$0")"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
QEMU="$REPO_ROOT/qemu-src/build/qemu-system-arm"

if [ $# -ne 1 ]; then
    echo "Usage: $0 <flash_image>"
fi

flash_image=$1

case "$flash_image" in
    *hi351[68][acde]v[0-9][0-9][0-9]*)
        soc=$(basename $flash_image | grep -o hi351[68][acde]v[0-9][0-9][0-9])
        ;;
    *gk720[0-9]v[0-9][0-9][0-9]*)
        soc=$(basename $file_image | grep -o gk720[0-9]v[0-9][0-9][0-9])
        ;;
    *sc6152*)
        soc=gk7205v200
        ;;
    *)
        echo "Unknow SoC Type!"
        exit 1
        ;;
esac

net_opt="-netdev tap,id=net0,ifname=tap0,script=no,downscript=no -net nic,netdev=net0"

# TODO: add non-interactive test cases
run_qemu="$QEMU -M $soc,flash-file=$flash_image -nographic $net_opt"

echo "Running $run_qemu"
$run_qemu
