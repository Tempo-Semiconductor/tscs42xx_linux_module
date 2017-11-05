#!/bin/sh

root="$PWD"
kernel_src_dir="/usr/src/linux-headers-$(uname -r)"

echo "Kernel source directory is: $kernel_src_dir/"

echo "Installing modules in $root/sound/soc/codecs/"
cd "$root/sound/soc/codecs/"
make -C $kernel_src_dir M=$PWD modules_install

echo "Installing modules in $root/sound/soc/bcm/"
cd "$root/sound/soc/bcm/"
make -C $kernel_src_dir M=$PWD modules_install

depmod -a

echo "Installing overlays"
cd "$root/arch/arm/boot/dts/overlays/"
cp rpi-tscs42xx-overlay.dtbo /boot/overlays/
