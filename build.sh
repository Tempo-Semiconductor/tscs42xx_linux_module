#!/bin/sh

root="$PWD"
kernel_src_dir="/usr/src/linux-headers-$(uname -r)"

echo "Kernel source directory is: $kernel_src_dir/"

echo "Building modules in $root/sound/soc/codecs/"
cd "$root/sound/soc/codecs/"
make -C $kernel_src_dir M=$PWD

echo "Building modules in $root/sound/soc/bcm/"
cd "$root/sound/soc/bcm/"
make -C $kernel_src_dir M=$PWD

echo "Building overlay"
cd "$root/arch/arm/boot/dts/overlays/"
dtc -@ -I dts -O dtb -o rpi-tscs42xx-overlay.dtbo rpi-tscs42xx-overlay.dts
