#!/bin/bash

set -e

rmmod sbdd || echo "module isn't loaded, no need to remove"
insmod sbdd.ko 

mkfs.ext4 /dev/sbdd
mkdir test_dir
mount /dev/sbdd test_dir
mkdir -p test_dir/d1/d2
echo "OK" > test_dir/d1/d2/f

cat test_dir/d1/d2/f
umount test_dir
rmdir test_dir
