# PSBBN Definitive Patch Kernel
Linux Kernel 2.4.17 for PS2 PSBBN with modifications for [PSBBN Definitive Patch](https://github.com/CosmicScale/PSBBN-Definitive-English-Patch)

## Commands for compiling locally

Launch docker on your host machine

    docker run --rm -it akuhak/ps2-legacy-woody-slim:latest /bin/bash

Inside docker run folllowing commands:

    cd linux-2.4.17_ps2
    ./setup-ps2
    yes "" | make oldconfig || true
    make dep
    make -j"$(nproc)" vmlinux
    make -j"$(nproc)" modules
    make modules_install INSTALL_MOD_PATH=rootfs
    mkdir -p rootfs/boot
    cp vmlinux rootfs/boot/vmlinux-2.4.17_mvl21
    cp System.map rootfs/boot/System.map-2.4.17_mvl21

rootfs folder should contain ready to go files
