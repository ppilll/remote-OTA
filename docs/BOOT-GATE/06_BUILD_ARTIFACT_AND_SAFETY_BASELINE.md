# 构建、产物与安全基线

## SDK 配置基线

```text
SDK root:
/home/liu2004/work/Linux_SDK/atk_dlrk3588_linux5.10

active defconfig:
01_atk_dlrk3588_auto2mipi_2hdmi_bootgate_ab_defconfig

RK_AB_UPDATE=y
RK_PARAMETER=parameter-ab.txt
RK_UBOOT_CFG=alientek_rk3588
RK_UBOOT_CFG_FRAGMENTS=alientek_rk3588-ab
RK_BUILDROOT_CFG=alientek_rk3588
RK_KERNEL_CFG=alientek_rk3588_defconfig
RK_ROOTFS_SYSTEM=buildroot
RK_ROOTFS_TYPE=ext4
```

U-Boot fragment：

```text
u-boot/configs/alientek_rk3588-ab.config
```

冻结内容：

```config
CONFIG_BASE_DEFCONFIG="alientek_rk3588_defconfig"
CONFIG_ANDROID_AB=y
# CONFIG_CMD_ANDROID_AB_SELECT is not set
```

不要替换为 vendor 通用 `rk3588-ab.config`，因为其 base 指向通用 `rk3588_defconfig`，可能丢失 Alientek 板级配置。

## U-Boot 构建基线

实际配置合并语义已在构建日志中出现：

```text
make alientek_rk3588_defconfig alientek_rk3588-ab.config
```

最终 U-Boot 配置：

```text
CONFIG_ANDROID_AB=y
CONFIG_SPL_AB=y
CONFIG_AVB_LIBAVB_AB=y
CONFIG_ROCKCHIP_FIT_IMAGE=y
# CONFIG_SPL_KERNEL_BOOT is not set
CONFIG_BASE_DEFCONFIG="alientek_rk3588_defconfig"
CONFIG_DEFAULT_DEVICE_TREE="rk3588-alientek"
```

结论：Alientek base 保留、A/B U-Boot/SPL 构建成功，为 `SDK BUILD VERIFIED`。

目标 cmdline 中运行版本标识包含：

```text
uboot-23c0020-di-09/03/2026
```

这提供了 matching build 的目标侧证据，但本冻结记录没有保存完整 commit hash 和全部 artifact SHA-256；复现时应重新采集。

## Offline artifact 基线

已观察到的 firmware staging：

```text
output/firmware/boot.img          -> ../../kernel/boot.img
output/firmware/MiniLoaderAll.bin -> ../../u-boot/rk3588_spl_loader_v1.17.113.bin
output/firmware/parameter.txt     -> ../../device/rockchip/.chips/rk3588/parameter-ab.txt
output/firmware/rootfs.img        -> ../../buildroot/output/alientek_rk3588/images/rootfs.ext2
output/firmware/uboot.img         -> ../../u-boot/uboot.img
output/firmware/update-ab.img     -> ../update-ab/Image/update.img
output/firmware/update.img        -> update-ab.img
```

构建日志确认：

```text
buildroot/rootfs succeeded
build_firmware succeeded
build_updateimg succeeded
```

实际 staging parameter 定义：

```text
uboot, misc, boot_a, boot_b, backup, system_a, system_b, oem, userdata
```

这使 active parameter 和 staging image 达到 `OFFLINE ARTIFACT VERIFIED`。

本地 packing 脚本源码已确认完整 A/B update 的设计映射：

```text
boot_a   -> boot.img
boot_b   -> boot.img
system_a -> rootfs.img
system_b -> rootfs.img
```

现有对话证据没有保留最终 `package-file` 全文或完整 hash 清单。不要为补齐文档而推测具体 hash。后续可从保留的 SDK output 重新采集。目标板已实际启动过 A 和 B，因此部署后的两个 boot/system 组都具有 `TARGET VERIFIED` 的可启动证据。

## 首次刷写基线

首次 single-slot -> A/B 迁移使用完整 `update.img`，由完整包同时迁移 matching loader/SPL、U-Boot、GPT、boot A/B 和 system A/B。不要用逐分区手工地址方式复现这次迁移。

## 冻结的禁止事项

- 不把整个 `BOOT-GATE` 标成 closed。
- 不在本阶段重新设计 U-Boot、SPL、AvbABData 或 GPT。
- 不切到 `fw_setenv/BOOT_ORDER` 方案；已验证的平台 authority 是 Rockchip Native A/B metadata。
- 不把 factory U-Boot 与当前 A/B firmware 混用。
- 不手工编辑 `u-boot/.config`；它是生成物。
- 不用 `dd`、shell seek 或 Agent 直接写 raw partition/misc 作为产品路径。
- 不在 RAUC 配置中硬编码 `/dev/mmcblk0p6` 之类的分区号；使用 `/dev/disk/by-partlabel/...`。
- 不以 ext4 filesystem UUID 判断当前槽；必须使用 GPT PARTUUID 与根挂载身份。
- 不把 inactive、从未启动的槽标为 successful-good。
- 不在缺少健康检查时自动 mark-good。
- 不把安装成功等同于新系统健康。
- 不在 backend/slot topology 未验证前启动 Remote OTA 自动 `rauc install + reboot`。
- 不因正常 A/B 回退成功就声称 Loader/Maskrom 恢复路径已验证。
- 不把缺失的 package-file、hash 或 commit 具体值补写成事实。

## 物理恢复状态

对话中确认了完整镜像已由 RKDevTool 刷入实板并成功启动；这证明实际目标部署。没有单独保存以下实验结果：

```text
进入 Loader/MASKROM 被工具识别
从 Loader/MASKROM 完成恢复刷写
恢复后重新启动
```

所以 recovery path 必须保持 `NOT HARDWARE VERIFIED`，直到完成并记录这些动作。

