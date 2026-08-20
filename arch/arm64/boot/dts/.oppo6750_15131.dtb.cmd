cmd_arch/arm64/boot/dts/oppo6750_15131.dtb := aarch64-linux-android-gcc -E -Wp,-MD,arch/arm64/boot/dts/.oppo6750_15131.dtb.d.pre.tmp -nostdinc -I/app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/arch/arm64/boot/dts -I/app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/arch/arm64/boot/dts/include -Iarch/arm64/boot/dts -I/app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/include/ -I/app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/drivers/of/testcase-data -undef -D__DTS__ -x assembler-with-cpp -o arch/arm64/boot/dts/.oppo6750_15131.dtb.dts.tmp arch/arm64/boot/dts/oppo6750_15131.dts ; /app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/scripts/dtc/dtc -O dtb -o arch/arm64/boot/dts/oppo6750_15131.dtb -b 0 -i arch/arm64/boot/dts/ -i /app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/drivers/misc/mediatek/mach/mt6755/oppo6750_15131/dct/dct/ -d arch/arm64/boot/dts/.oppo6750_15131.dtb.d.dtc.tmp arch/arm64/boot/dts/.oppo6750_15131.dtb.dts.tmp ; cat arch/arm64/boot/dts/.oppo6750_15131.dtb.d.pre.tmp arch/arm64/boot/dts/.oppo6750_15131.dtb.d.dtc.tmp > arch/arm64/boot/dts/.oppo6750_15131.dtb.d

source_arch/arm64/boot/dts/oppo6750_15131.dtb := arch/arm64/boot/dts/oppo6750_15131.dts

deps_arch/arm64/boot/dts/oppo6750_15131.dtb := \
  arch/arm64/boot/dts/mt6755.dtsi \
    $(wildcard include/config/addr.h) \
    $(wildcard include/config/base.h) \
    $(wildcard include/config/mtk/legacy.h) \
  /app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/arch/arm64/boot/dts/include/dt-bindings/clock/mt6755-clk.h \
  /app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/arch/arm64/boot/dts/include/dt-bindings/interrupt-controller/arm-gic.h \
  /app/conversations/6a807cb012b12b33001d2a7d/oppo_f1s_rom/kernel/oppo/kernel-3.10/arch/arm64/boot/dts/include/dt-bindings/interrupt-controller/irq.h \
  arch/arm64/boot/dts/mt6755-pinfunc.h \
  arch/arm64/boot/dts/mt65xx.h \
  arch/arm64/boot/dts/mt6353.dtsi \
  arch/arm64/boot/dts/cust_eint.dtsi \
  arch/arm64/boot/dts/cust_i2c.dtsi \
  arch/arm64/boot/dts/cust_kpd.dtsi \
  arch/arm64/boot/dts/cust_clk_buf.dtsi \
  arch/arm64/boot/dts/cust_md1_eint.dtsi \

arch/arm64/boot/dts/oppo6750_15131.dtb: $(deps_arch/arm64/boot/dts/oppo6750_15131.dtb)

$(deps_arch/arm64/boot/dts/oppo6750_15131.dtb):
