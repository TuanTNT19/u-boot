#!/bin/bash

PI4_OWRT_DEFCONFIG="configs/rpi_4_owrt_defconfig"

help()
{
    echo "Usage:"
    echo "  $0 <defconfig>"
    echo "  $0 <defconfig> <PARTUUID>"
    echo "  $0 set-partuuid <PARTUUID>"
    echo "  $0 -h | --help | help"
    echo
    echo "Description:"
    echo "  Build U-Boot using the specified defconfig."
    echo "  PARTUUID can optionally be provided to update CONFIG_BOOTARGS"
    echo "  before building."
    echo
    echo "Examples:"
    echo "  With pi4 owrt:"
    echo "  $0 rpi_4_owrt_defconfig"
    echo "      Build U-Boot without changing PARTUUID."
    echo
    echo "  $0 rpi_4_owrt_defconfig 3205d7aa"
    echo "      Set PARTUUID=3205d7aa and build U-Boot."
    echo
    echo "  $0 set-partuuid 3205d7aa"
    echo "      Only update PARTUUID in ${PI4_OWRT_DEFCONFIG}."
    echo "  With other device:"
    echo "  $0 <board>s_defconfig"
}

set_partuuid()
{
    local PARTUUID="$1"

    if [ -z "${PARTUUID}" ]; then
        echo "Error: PARTUUID is required"
        echo "Usage: $0 set-partuuid <PARTUUID>"
        exit 1
    fi

    echo "Setting PARTUUID=${PARTUUID}"

    sed -i \
        "s#^CONFIG_BOOTARGS=.*#CONFIG_BOOTARGS=\"console=tty1 console=serial0,115200 root=PARTUUID=${PARTUUID}-02 rootfstype=squashfs,ext4 rootwait\"#" \
        "${PI4_OWRT_DEFCONFIG}"
}

build()
{
    local DEFCONFIG_NAME="$1"

    if [ -z "${DEFCONFIG_NAME}" ]; then
        echo "Error: defconfig is required"
        echo "Use '$0 --help' for more information"
        exit 1
    fi

    make distclean
    make "${DEFCONFIG_NAME}"
    make -j$(nproc) \
        CROSS_COMPILE=aarch64-linux-gnu- \
        CC=aarch64-linux-gnu-gcc-10

    mkdir -p out
    cp u-boot.bin out/
}

# Help
if [ "$1" = "help" ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    help
    exit 0
fi

# Mode 1: Only set PARTUUID
if [ "$1" = "set-partuuid" ]; then
    set_partuuid "$2"
    exit 0
fi

# Mode 2: Set PARTUUID and build
if [ -n "$2" ]; then
    set_partuuid "$2"
fi

# Mode 3: Normal build
build "$1"