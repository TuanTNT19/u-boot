#!/bin/bash

make distclean

make ${1}

make -j$(nproc) CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc-10

mkdir -p out

cp u-boot.bin out/

