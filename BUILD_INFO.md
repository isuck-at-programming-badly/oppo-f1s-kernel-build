# Oppo F1s Custom ROM & Kernel Build

## ROM: Android 10 (LineageOS 17.1) - v2 FIXED
- **File:** OppoF1s_Android10_v2_FIXED.zip (572.5MB)
- **Base:** LineageOS 17.1 for Oppo A37 (MT6750, same chipset as F1s)
- **Kernel:** Custom kernel-3.10 with all optimizations (swapped into boot.img)
- **Boot logo:** Custom AOSP boot logo (flashed to logo partition)
- **Root:** Magisk v23.0 (installed via updater-script)

## v2 Fixes (from v1)
1. Removed device assertion that blocked F1s (only allowed a37f/A37fw)
2. Added boot logo flashing to logo partition
3. Added Magisk installation to updater-script
4. Added A1601 to metadata pre-device list

## Kernel: Build 29
- **Image:** Image.gz-dtb (7.7MB, inserted into 8.6MB boot.img)
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
2. Backup ALL partitions (CRITICAL!)
3. Wipe: System, Cache, Dalvik, Data
4. Flash OppoF1s_Android10_v2_FIXED.zip
5. Flash Android 10 GApps (arm64) if you need Google apps
6. Reboot system
7. First boot takes 5-10 minutes

## Updater Script Flow
1. Extract install files
2. Backup system partition
3. Patch system image (block_image_update)
4. Restore system backup
5. Flash custom kernel (boot.img -> boot partition)
6. Flash custom boot logo (logo.bin -> logo partition)
7. Install Magisk v23.0

## Fixes Applied During Build
- LZ4K: removed #ifdef CONFIG_UBIFS_FS guard
- xlog: added no-op stubs for disabled CONFIG_HAVE_XLOG_PRINTK
- GPIO: restored cust_gpio_usage.h with all DCT pin definitions
- GPIO: added GPIO_OTG_DRVVBUS_PIN stub for OTG VBUS
- Touchscreen: added missing GPIO defines to focaltech driver
- Scheduler: added debug_stubs.c for missing debug functions
- BFQ: fixed IOPRIO_PRIO_DEFAULT definition
