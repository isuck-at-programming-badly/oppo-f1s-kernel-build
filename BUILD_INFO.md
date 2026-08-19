# Oppo F1s Custom Kernel - Build 29

## Build Details
- **Date:** 2026-08-19
- **Kernel:** 3.10.x (MTK MT6755)
- **Device:** Oppo F1s (A1601)
- **Image:** Image.gz-dtb (7.7MB)
- **vmlinux:** 23MB
- **Flashable ZIP:** OppoF1s_CustomKernel_Build29.zip (11MB)

## Features
- Custom governors: blu_active, interactiveplus
- Custom I/O schedulers: BFQ, Zen, Maple
- TCP Westwood congestion control
- ZRAM with LZ4K compression
- NetHunter framework support
- KernelSU integration
- Cortex-A53 optimizations (-O3)
- VM tuning (swappiness=30, cache_pressure=50)
- Custom boot logo support
- OTG/USB support

## Flashing
1. Boot into TWRP recovery
2. Flash OppoF1s_CustomKernel_Build29.zip
3. Reboot
