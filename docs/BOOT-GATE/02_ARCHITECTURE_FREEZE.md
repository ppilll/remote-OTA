# 架构冻结

## 平台基线

```text
Board:       正点原子 ATK-DLRK3588
SoC:         Rockchip RK3588
Arch:        AArch64
Storage:     eMMC
OS:          Buildroot 2021.11 family
Kernel:      Linux 5.10.209 family
U-Boot:      Rockchip vendor U-Boot 2017.09 family
SDK path:    /home/liu2004/work/Linux_SDK/atk_dlrk3588_linux5.10
SDK manifest: atk-rk3588_linux_release_v2.0_20260703.xml
SDK version: linux-5.10-gen-rkr8
```

## 冻结的启动链

```text
BootROM / loader
  -> SPL
  -> U-Boot proper
  -> Rockchip AvbABData selects slot
  -> boot_a or boot_b FIT image
  -> kernel + DTB
  -> system_a or system_b selected by GPT PARTUUID
  -> Buildroot userspace
```

SPL A/B 与 U-Boot proper A/B 必须区分：

```text
SPL:           CONFIG_SPL_AB=y
U-Boot proper: CONFIG_ANDROID_AB=y
Linux boot owner is not SPL because CONFIG_SPL_KERNEL_BOOT is disabled.
```

不能从 `CONFIG_SPL_AB=y` 单独推导 Linux A/B 已完成。

## U-Boot proper 的 slot/rootfs 链

本地 vendor SDK 已确认的源码链：

```text
bootargs_add_android()
  -> ab_update_root_partition()
  -> part_get_info_by_name("system")
  -> rk_avb_append_part_slot()
  -> system_a or system_b
  -> get_partition_unique_uuid()
  -> root=PARTUUID=<selected system partition GUID>
```

FIT 启动路径同时产生 vendor 实际字段：

```text
android_slotsufix=_a
android_slotsufix=_b
```

拼写必须保持 `android_slotsufix`（一个 `f`）。不要未经目标证据改用其他 Android slot 参数。

## Boot-control metadata

```text
device:  /dev/disk/by-partlabel/misc
offset:  2048 bytes
size:    32 bytes
format:  Rockchip-AvbABData
version: 1.0
CRC:     stored metadata was verified as valid on target
```

Rockchip 状态是三态：

| Native 状态 | 条件 | 含义 |
|---|---|---|
| confirmed-good | `successful=1, tries=0` | 已实际启动并通过健康确认 |
| pending | `successful=0, tries>0, priority>0` | 可启动但未确认；启动会消耗 retry |
| bad/unbootable | `successful=0, tries=0`，canonical 形式 `priority=0` | bootloader 不应再选择 |

## 目标 GPT 分区基线

| 分区 | 设备 | 大小 | 起始 sector | 冻结用途 |
|---|---|---:|---:|---|
| `uboot` | `/dev/mmcblk0p1` | 4 MiB | `0x00004000` | matching U-Boot proper |
| `misc` | `/dev/mmcblk0p2` | 4 MiB | `0x00006000` | A/B metadata 等 |
| `boot_a` | `/dev/mmcblk0p3` | 64 MiB | `0x00008000` | A 组 FIT boot image |
| `boot_b` | `/dev/mmcblk0p4` | 128 MiB | `0x00028000` | B 组 FIT boot image |
| `backup` | `/dev/mmcblk0p5` | 32 MiB | `0x00068000` | vendor reserved/backup |
| `system_a` | `/dev/mmcblk0p6` | 7 GiB | `0x00078000` | A 组 ext4 rootfs |
| `system_b` | `/dev/mmcblk0p7` | 7 GiB | `0x00e78000` | B 组 ext4 rootfs |
| `oem` | `/dev/mmcblk0p8` | 128 MiB | `0x01c78000` | OEM data |
| `userdata` | `/dev/mmcblk0p9` | grow | `0x01cb8000` | persistent user data |

`boot_a` 与 `boot_b` 大小不对称是 vendor `parameter-ab.txt` 的实际定义。没有镜像尺寸或工具链证据时不要擅自“修正”为对称布局。

已观察到的关键 PARTUUID：

```text
system_a: 4b1a0000-0000-4208-8000-4c2e00003e31 -> /dev/mmcblk0p6
system_b: c3180000-0000-4d5b-8000-5dbf00004e66 -> /dev/mmcblk0p7
```

首次构建把同一个 rootfs image 写入 A/B，所以两个 ext4 filesystem UUID 相同是预期行为。判定当前槽必须使用 GPT PARTUUID 和 block-device identity，不能使用 ext4 filesystem UUID。

## 职责边界

```text
Remote OTA Agent = 编排、策略、报告
RAUC             = bundle 验签、inactive slot group 安装、更新生命周期
Custom backend   = RAUC boot semantics <-> Rockchip AvbABData
U-Boot/SPL       = 实际选槽、retry、fallback
```

Remote OTA Agent 不直接写 `/dev/mmcblk*`，不自己 `dd` rootfs，不直接操作 `misc`，不硬编码分区号。

