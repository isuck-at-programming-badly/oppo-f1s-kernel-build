# Oppo F1s Custom ROM & Kernel Build

## ROM: Android 10 (LineageOS 17.1)
- **File:** OppoF1s_Android10_CustomROM_v1.zip (572.5MB)
- **Base:** LineageOS 17.1 for Oppo A37 (MT6750, same chipset as F1s)
- **Kernel:** Custom kernel-3.10 with all optimizations
- **Boot logo:** Custom AOSP boot logo
- **Root:** Magisk v23.0 included

## Kernel: Build 29
- **Image:** Image.gz-dtb (7.7MB)
- **vmlinux:** 23MB
- **Zero compilation errors**

## Features
- Custom governors: blu_active, interactiveplus
- Custom I/O schedulers: BFQ, Zen, Maple
- TCP Westwood congestion control
- ZRAM with LZ4K compression
- NetHunter framework support (27 kernel modules)
- KernelSU integration
- Cortex-A53 optimizations (-O3)
- VM tuning (swappiness=30, cache_pressure=50)
- Custom boot logo (6 layers)

## Flashing Instructions
1. Boot into TWRP recovery (Vol-Up + Power)
2. Backup all partitions (CRITICAL)
3. Wipe: System, Cache, Dalvik, Data
4. Flash OppoF1s_Android10_CustomROM_v1.zip
5. Flash Magisk-v23.0.apk (optional, for root)
6. Flash Android 10 GApps (arm64) if you need Google apps
7. Reboot system
8. First boot takes 5-10 minutes

## Download
ROM: See upload link
Kernel only: OppoF1s_CustomKernel_Build29.zip (11MB, in this repo)

## Fixes Applied During Build
- LZ4K: removed #ifdef CONFIG_UBIFS_FS guard
- xlog: added no-op stubs for disabled CONFIG_HAVE_XLOG_PRINTK
- GPIO: restored cust_gpio_usage.h with all DCT pin definitions
- GPIO: added GPIO_OTG_DRVVBUS_PIN stub for OTG VBUS
- Touchscreen: added missing GPIO defines to focaltech driver
- Scheduler: added debug_stubs.c for missing debug functions
- BFQ: fixed IOPRIO_PRIO_DEFAULT definition
