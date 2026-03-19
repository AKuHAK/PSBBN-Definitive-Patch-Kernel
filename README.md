# PSBBN Linux Kernel

Linux Kernel 2.4.17 for PlayStation 2 Broadband Navigator (PSBBN).

## Documentation for AI Agents & Developers

For comprehensive architecture details, build procedures, subsystem guides, and development rules, please refer to **[AGENTS.md](AGENTS.md)**.

## Commands for compiling locally

Launch docker on your host machine:

```bash
docker run --rm -it -v "$(pwd)":/usr/src/project -w /usr/src/project akuhak/ps2-legacy-woody-slim:latest /bin/bash
```

Inside docker run following commands:

```bash
./setup-ps2
yes "" | make oldconfig || true
make dep
make -j"$(grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 2)" vmlinux
make -j"$(grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 2)" modules
make modules_install INSTALL_MOD_PATH=rootfs
mkdir -p rootfs/boot
cp vmlinux rootfs/boot/vmlinux-2.4.17_mvl21
cp System.map rootfs/boot/System.map-2.4.17_mvl21
```

The `rootfs` folder will contain the ready-to-use kernel and modules.
