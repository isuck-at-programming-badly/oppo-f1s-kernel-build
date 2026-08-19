# Oppo F1s Custom Kernel (MT6750/MT6755)

Custom Android kernel for the **Oppo F1s (A1601)** based on the MediaTek MT6750/MT6755 SoC.

## Build Instructions

```bash
# Set up toolchain
export ARCH=arm64
export CROSS_COMPILE=/path/to/aarch64-linux-android-

# Configure
make oppo6750_15131_defconfig

# Build
make -j$(nproc)
```

The output kernel image will be at `arch/arm64/boot/Image.gz-dtb`.

## Flashable ZIP

Use the AnyKernel3 directory to create a flashable TWRP ZIP:

```bash
cd AnyKernel3
# Copy built kernel
cp ../kernel/oppo/kernel-3.10/arch/arm64/boot/Image.gz-dtb kernel/
# Create ZIP
zip -r ../oppo-f1s-custom-kernel.zip * -x "*.git*"
```

Flash the resulting ZIP in TWRP recovery.

## Features

### Performance
- **CPU Governors**: interactive (default), ondemand, powersave, conservative, interactiveplus, blu_active
- **I/O Schedulers**: noop, deadline (default), cfq, BFQ, zen, maple
- **TCP Congestion**: westwood (default), cubic
- **Compiler**: -O3 optimization with Cortex-A53 tuning

### Memory
- **ZRAM**: Enabled with LZ4K compression
- **VM tweaks**: swappiness=30, vfs_cache_pressure=50

### Security
- KernelSU-style root support (where available)

### NetHunter Support
- USB HID keyboard/mouse emulation
- USB OTG network adapters (RTL8152, RNDIS, CDC)
- Wireless monitor mode (mac80211)
- Netfilter packet injection

### Display & Battery
- Double Tap to Wake (DT2W) via Focaltech driver
- MTK SPM power optimization
- Reduced standby battery drain
- Custom boot logo

### Fixes Applied
- Custom Python DrvGen replacement for broken 32-bit MediaTek tool
- DCT header generation (GPIO, EINT, KPD, clock buffers, PMIC)
- Magnetometer AKM09911 struct member fix
- xhci USB driver: missing return value, function prototype, IDDIG GPIO mode
- Charger IC include path fix
- EINT constants for fingerprint sensor
- Removed OEM debug/logging overhead (DLOG, MTK_LOG)

## Device Info
- **SoC**: MediaTek MT6750/MT6755 (Helio P10)
- **CPU**: 4x Cortex-A53 @1.0GHz + 4x Cortex-A53 @1.5GHz (big.LITTLE)
- **RAM**: 3GB/4GB
- **Display**: 5.5" 720x1280 LCD
- **Kernel**: Linux 3.10.x
- **Android**: 5.1 (Lollipop)

## Branch Strategy
- `main` - Stable, functional builds
- `feature/governors` - CPU governor development
- `feature/nethunter` - NetHunter/penetration testing features
- `feature/schedulers` - I/O scheduler development

## Credits
- MediaTek for the base kernel source
- AnyKernel3 by osm0sis
- The Android kernel community for governor patches
