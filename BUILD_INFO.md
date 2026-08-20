# Oppo F1s Custom Kernel - GSI Ready (Build 30)

## Kernel: v3 (Build 30)
- **Image:** Image.gz-dtb (7.7MB)
- **Zero compilation errors** across 28,000+ lines

## What's New in v3 (GSI-critical)
- CONFIG_FHANDLE=y (file handle ops for Android 10)
- CONFIG_TMPFS_POSIX_ACL=y (SELinux permissions)
- CONFIG_TMPFS_XATTR=y (extended attributes)
- MTK-fixed boot cmdline (was Qualcomm, now MediaTek)
- Verity disabled, SELinux permissive, vbmeta unlocked

## Flashable ZIP
- **File:** OppoF1s_GSI_Boot_v3.zip (8.5MB)
- Flashes: boot.img (custom kernel) + logo.bin (custom boot logo)

## GSI Download (phh-treble Android 10)
- Vanilla: https://github.com/phhusson/treble_experimentations/releases/download/v222/system-quack-arm32_binder64-aonly-vanilla.img.xz
- GApps: https://github.com/phhusson/treble_experimentations/releases/download/v222/system-quack-arm32_binder64-aonly-gapps.img.xz
- Image type: arm32_binder64-aonly (32-bit userspace, 64-bit binder, A-only partition)

## Flashing Steps
1. Flash OppoF1s_GSI_Boot_v3.zip in TWRP (kernel + logo)
2. Download GSI, extract .img, flash to system partition
3. Wipe data, cache, dalvik
4. Reboot (first boot 5-10 min)

## Boot Cmdline (MTK-fixed)
```
console=tty0 console=ttyMT3,921600n1 root=/dev/ram vmalloc=496M
slub_max_order=0 androidboot.hardware=mt6735
androidboot.bootdevice=mtk-msdc.0
androidboot.selinux=permissive
androidboot.veritymode=disabled
androidboot.vbmeta.device_state=unlocked
buildvariant=userdebug
```

## Kernel Features
- Custom governors: blu_active, interactiveplus
- Custom I/O schedulers: BFQ, Zen, Maple
- TCP Westwood congestion control
- ZRAM with LZ4K compression
- NetHunter framework (27 modules)
- KernelSU integration
- Cortex-A53 -O3 optimizations
- Loop device support (for GSI vendor.img)
- Seccomp filter (Android 10 requirement)
- UINPUT (virtual input for phh-treble)

## Known Issues with A37 ROM (v1/v2)
- A37 system image has wrong hardware configs (Qualcomm cmdline, A37 panel/touch/camera)
- Would NOT boot on F1s
- Replaced with GSI approach (hardware-agnostic system image)

## Fixes Applied During Build
- LZ4K: removed #ifdef CONFIG_UBIFS_FS guard
- xlog: added no-op stubs for disabled CONFIG_HAVE_XLOG_PRINTK
- GPIO: restored cust_gpio_usage.h with DCT pin definitions
- GPIO: added GPIO_OTG_DRVVBUS_PIN stub for OTG VBUS
- Touchscreen: added missing GPIO defines to focaltech driver
- Scheduler: added debug_stubs.c for missing debug functions
- BFQ: fixed IOPRIO_PRIO_DEFAULT definition
