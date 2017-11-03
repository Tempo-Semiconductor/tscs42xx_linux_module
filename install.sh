#!/bin/sh

root="$PWD"
kernel_src_dir="/usr/src/linux-headers-$(uname -r)"
#codec_mods_dir="kernel/sound/soc/codecs"
#sound_card_mods_dir="kernel/sound/soc/bcm"

echo "Kernel source directory is: $kernel_src_dir/"

echo "Installing modules in $root/sound/soc/codecs/"
cd "$root/sound/soc/codecs/"
make -C $kernel_src_dir M=$PWD modules_install

echo "Installing modules in $root/sound/soc/bcm/"
cd "$root/sound/soc/bcm/"
make -C $kernel_src_dir M=$PWD modules_install

depmod -a
echo dtoverlay=rpi-tscs42xx-overlay >> /boot/config.txt

echo "Installing overlays"
cd "$root/arch/arm/boot/dts/overlays/"
cp rpi-tscs42xx-overlay.dtbo /boot/overlays/
