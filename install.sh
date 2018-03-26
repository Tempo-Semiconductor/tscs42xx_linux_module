#!/bin/bash

root="$PWD"
kernel_src_dir="/usr/src/linux-headers-$(uname -r)"
install_all=true
install_codec=false
install_overlay=false
run_depmod=false
make_args=''

options=':d:i:a:h'

usage_str="$(basename "$0") [-h] [-d dir] [-i target] [-a args] -- Builds the TSCS42xx kernel module

where:
    -h  show this help text
    -d  set the source directory
    -i  set the install targets codec or overlay
    -a  arguments to pass to make"

usage() { echo "$usage_str"; }

while getopts $options opt; do
    case ${opt} in
    d )
        kernel_src_dir="${OPTARG}"
        ;;
    i )
        install_all=false
        echo "$OPTARG"
        if [ "$OPTARG" = 'codec' ]; then
            install_codec=true
        elif [ "$OPTARG" = 'overlay' ]; then
            install_overlay=true
        else
            echo -e "\n *** Error: Unrecognized install target $OPTARG\n" >&2; usage; exit 1;
        fi
        ;;
    a )
        make_args="${OPTARG}"
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

if [ "$install_all" = true ] || [ "$install_codec" = true ]; then
    echo "Installing modules in $root/sound/soc/codecs/"
    cd "$root/sound/soc/codecs/"
    make $make_args -C $kernel_src_dir M=$PWD modules_install
    run_depmod=true
fi

if [ "$install_all" = true ] || [ "$install_overlay" = true ]; then
    echo "Installing overlays"
    cd "$root/arch/arm/boot/dts/overlays/"
    cp rpi-tscs42xx-overlay.dtbo /boot/overlays/
fi

if [ "$run_depmod" = true ]; then
    depmod -a
fi
