#!/bin/bash

set -e

if [[ $# -ne 1 ]]; then
	echo "usage: test.sh blk_device"
	exit 1
fi

BLK_DEVICE=$1

rmmod sbdd || echo "module isn't loaded, no need to remove"
insmod sbdd.ko device="${BLK_DEVICE}"

mkfs.ext4 /dev/sbdd
mkdir -p test_dir
mount /dev/sbdd test_dir
mkdir -p test_dir/d1/d2
echo "OK" > test_dir/d1/d2/f

cat test_dir/d1/d2/f
umount test_dir
rmmod sbdd

mount $BLK_DEVICE test_dir
cat test_dir/d1/d2/f
umount $BLK_DEVICE

rmdir test_dir
