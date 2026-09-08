#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
linux="$root/third_party/linux"

make -C "$linux" x86_64_defconfig
cfg="$linux/scripts/config"
$cfg --file "$linux/.config" \
	--enable 64BIT --enable X86_64 --enable PCI --enable PCI_DIRECT \
	--enable PCI_MMCONFIG --enable PCI_QUIRKS --enable DEVTMPFS \
	--enable DEVTMPFS_MOUNT --enable BLK_DEV_INITRD --enable RD_GZIP \
	--enable BINFMT_ELF --enable BINFMT_SCRIPT --enable TTY --enable VT \
	--enable SERIAL_8250 --enable SERIAL_8250_CONSOLE --enable PRINTK \
	--enable PRINTK_TIME --enable PROC_FS --enable SYSFS --enable TMPFS \
	--enable MODULES --enable MODULE_UNLOAD --enable ELF_CORE --enable COREDUMP \
	--enable FUTEX --enable EPOLL --enable SIGNALFD --enable TIMERFD \
	--enable EVENTFD --enable SHMEM --enable DEBUG_FS
$cfg --file "$linux/.config" \
	--enable NET --enable INET --enable PACKET --enable UNIX --enable NETDEVICES \
	--enable ETHERNET --enable NET_VENDOR_INTEL --enable E1000 --enable SYSVIPC \
	--enable HUGETLBFS --enable HUGETLB_PAGE --enable NUMA --enable SMP
make -C "$linux" olddefconfig
