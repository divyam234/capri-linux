# Linux for Motorola Moto G10 and Moto G10 Power

Mainline-based **Linux 7.4-rc3** kernel tree for the Motorola Moto G10
(`capri`, XT2127-2) and Moto G10 Power (XT2127-4), based on the Qualcomm
Snapdragon 460 / SM4250.

This is an **experimental bring-up tree**, not a released, device-tested
kernel. The original G10 drivers have been ported to Linux 7.4, and a
separate G10 Power device tree and Qualcomm PMI632 battery stack have been
integrated. Neither variant has been boot-validated with this exact 7.4 tree.
In particular, **charging behavior has not been validated on hardware**.

## Devices and device trees

| Device | DTB to boot | Battery profile | Board ID |
| --- | --- | --- | --- |
| Moto G10 XT2127-2 | `sm4250-motorola-capri.dtb` | JK50, 4,850 mAh / 4.40 V | `0x0 0xb200` |
| Moto G10 Power XT2127-4, DVT2 SKU5 | `sm4250-motorola-capri-power.dtb` | ATL4864356, 6,000 mAh / 4.45 V | `0x500 0xb200` |

The **Power DTS inherits the complete G10 DTS**, overriding only
Power-specific properties such as the battery data, board ID and Bluetooth
address. There are not two independent copies of the hardware configuration.

The stock Power EVT SKU5 overlay uses a different board ID
(`0x500 0xa000`). **Check the actual phone revision before booting**; the
provided Power DTB targets DVT2 SKU5. Other revisions, displays and
component substitutions have not been verified.

Main board files:

- `arch/arm64/boot/dts/qcom/sm4250-motorola-capri.dts`
- `arch/arm64/boot/dts/qcom/sm4250-motorola-capri-power.dts`
- `arch/arm64/boot/dts/qcom/capri-battery.dtsi`
- `arch/arm64/boot/dts/qcom/capri-power-battery.dtsi`
- `arch/arm64/boot/dts/qcom/capri-battery-therm.dtsi`

## Hardware bring-up status

**Integrated** means the code and/or device-tree description is present;
it does **not** mean the hardware works on Linux 7.4.

| Subsystem | Included in this tree | Linux 7.4 status |
| --- | --- | --- |
| Display and touchscreen | DJN NT36525B panel; Novatek touch controller | Ported; device test needed |
| GPU | Adreno 610 / DRM MSM | Ported; device test needed |
| Storage | eMMC (SDHC1), removable microSD (SDHC2 with GPIO88 card detect) | Configured; microSD insertion test needed |
| Wi-Fi | Qualcomm WCN3990 / ath10k integration | Ported; firmware and device test needed |
| Bluetooth | WCN3950 UART, power and suspend fixes | Ported; address provisioning and pairing test needed |
| USB | DWC3 controller and HS-only board configuration | Ported; host/device test needed |
| Audio | Qualcomm ASoC routing and Awinic speaker amplifier | Ported; playback/capture test needed |
| Cameras | S5KGM1ST, S5K4H7 and CAMSS; Linux 7.4 V4L2 callback updates | Source compatibility updated; device test needed |
| Fingerprint | Chipone interface and QSEECOM transport | Ported; firmware/userspace test needed |
| Battery | Native PMI632 SMB5 charger, QGauge and thermistor | Integrated; charging safety **unverified** |
| Suspend/resume | Capri platform and peripheral fixes | Needs extended device testing |
| Cellular | Modem integration incomplete | Not validated |

Earlier G10 results from older kernels must not be interpreted as
successful tests of this Linux 7.4 port.

## Battery and charging safety

The common device tree uses the Moto G10 **JK50** battery profile.
The Power overlay uses stock **ATL4864356** battery data, including a
6,000 mAh nominal design capacity, 4.45 V maximum voltage, 3 A factory
pack current limit, and 290 mA termination-current property.

The 3 A value describes the battery profile; **it is not an instruction
to force 3 A charging**. The Power OCV-to-percentage lookup is an
approximation at 25 °C, not the original temperature-dependent factory
fuel-gauge calibration.

The older custom `capri-pmi632-battery.c` source is retained in the
tree but **is not selected for the kernel build**. The active path is:

- `drivers/power/supply/qcom_smbx.c` — Qualcomm SMB5 / PMI632 charging
- `drivers/power/supply/qcom_pmi632_policy.h` — PMI632 protection policy
- `drivers/power/supply/qcom_qg.c` — Qualcomm battery monitoring
- `drivers/power/supply/qg-soc.h` — experimental charge-level estimator

**Do not enable or rely on unattended charging with this kernel** until
the actual handset has been checked for battery identification,
thermistor scaling, JEITA thresholds, charge-current and float-voltage
limits, watchdog operation, termination, and suspend/resume behavior.
Prior tests on another kernel version do not validate this port.

## Bluetooth address

The Power-specific DTB currently contains a
`local-bd-address` fallback copied from a local device configuration.
Some Qualcomm Bluetooth firmware reports an unprovisioned default address
and requires an explicitly provided address before `hci0` can be used.

**Do not distribute that address to multiple phones.** Replace it with
the actual handset's address or arrange per-device provisioning.
The regular G10 DTS does not receive this Power-only override.
Pairing, reconnection and Bluetooth audio still need testing.

## Build

Use a Linux host with the kernel build dependencies and a working
AArch64-capable LLVM/Clang toolchain. From this repository's root:

```sh
export ARCH=arm64

make LLVM=1 O=out motorola_capri_defconfig

make LLVM=1 O=out -j"$(nproc)" \
  Image modules \
  qcom/sm4250-motorola-capri.dtb \
  qcom/sm4250-motorola-capri-power.dtb
```

Expected build outputs:

```text
out/arch/arm64/boot/Image
out/arch/arm64/boot/dts/qcom/sm4250-motorola-capri.dtb
out/arch/arm64/boot/dts/qcom/sm4250-motorola-capri-power.dtb
```

To install modules into a prepared root filesystem:

```sh
make LLVM=1 O=out INSTALL_MOD_PATH=/path/to/rootfs modules_install
```

These are **build instructions, not a claim of a passing full build**.
Earlier checks compiled both DTBs and selected drivers before the native
charger/QGauge port; the current complete tree needs fresh compilation.

## Booting and firmware

Use the DTB matching the phone's **actual board revision**. Standard
AArch64 Linux accepts a device tree from the bootloader, so the older
custom U-Boot FDT-replacement patch is not included. U-Boot can provide
the kernel `Image`, the matching DTB and the distribution's initramfs;
boot image layout and memory properties must be checked for the handset.

**Keep a known-good boot and recovery path.** Do not overwrite working
boot partitions or flash an unverified kernel simply because it builds.

This repository supplies kernel source, **not proprietary device firmware
or a complete distribution**. Firmware is required for components such
as the touchscreen, GPU, modem, ADSP, Wi-Fi and Bluetooth. Relevant
board-specific paths include:

- `novatek/capri-djn-nt36525b.bin` — touchscreen
- `qcom/motorola/capri/a610_zap.mdt` — GPU secure firmware
- `modem.mdt` and `adsp2.mdt` — remote-processor firmware

Use firmware extracted for the matching device/stock release; user-space
audio, sensor and wireless services are also required.

## Kernel lineage

This branch starts at upstream **Linux v7.4-rc3**. Its single G10
integration commit combines 34 cherry-picked changes from
[Michael McAllister's Capri kernel](https://github.com/michaelmcallister/linux-motorola-capri)
(`capri-7.3`) and the Linux 7.4 camera callback fix. Three additional
source changes were already present upstream. The original commit list
and individual authors are preserved in the integration commit message.

Subsequent commits add native PMI632/QGauge support, the Power battery
and Bluetooth overlays, and microSD wiring. The older local charging
implementation was adapted from the user's existing Capri patches.

## License

Linux kernel code is licensed under the terms in [COPYING](COPYING)
and [LICENSES](LICENSES). Individual source files have their own SPDX
license identifiers.
