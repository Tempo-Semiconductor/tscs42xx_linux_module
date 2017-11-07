#!/bin/bash

root="$PWD"
kernel_src_dir="/usr/src/linux-headers-$(uname -r)"
build_all=true
build_codec=false
build_card=false
build_overlay=false

options=':d:b:h'

usage_str="$(basename "$0") [-h] [-d dir] [-b target] -- Builds the TSCS42xx kernel module

where:
    -h  show this help text
    -d  set the source directory
    -b  set the build targets codec, card, and/or overlay"

usage() { echo "$usage_str"; }

while getopts $options opt; do
    case ${opt} in
    d )
        kernel_src_dir="${OPTARG}"
        ;;
    b )
        build_all=false
        echo "$OPTARG"
        if [ "$OPTARG" = 'codec' ]; then
            build_codec=true
        elif [ "$OPTARG" = 'card' ]; then
            build_card=true
        elif [ "$OPTARG" = 'overlay' ]; then
            build_overlay=true
        else
            echo -e "\n *** Error: Unrecognized build target $OPTARG\n" >&2; usage; exit 1;
        fi
        ;;
    h )
        usage; exit 0;
        ;;
    \? )
        echo -e "\n *** Error: Unrecognized argument -$OPTARG\n" >&2; usage; exit 1;
        ;;
    : )
        echo -e "\n *** Error: Missing argument for -$OPTARG\n" >&2; usage; exit 1;
        ;;
    esac
done

echo "Kernel source directory: $kernel_src_dir/"

if [ "$build_all" = true ] || [ "$build_codec" = true ]; then
echo "Building modules in $root/sound/soc/codecs/"
cd "$root/sound/soc/codecs/"
make -C $kernel_src_dir M=$PWD modules
fi

if [ "$build_all" = true ] || [ "$build_card" = true ]; then
echo "Building modules in $root/sound/soc/bcm/"
cd "$root/sound/soc/bcm/"
make -C $kernel_src_dir M=$PWD modules
fi

if [ "$build_all" = true ] || [ "$build_overlay" = true ]; then
echo "Building overlay"
cd "$root/arch/arm/boot/dts/overlays/"
dtc -@ -I dts -O dtb -o rpi-tscs42xx-overlay.dtbo rpi-tscs42xx-overlay.dts
fi
