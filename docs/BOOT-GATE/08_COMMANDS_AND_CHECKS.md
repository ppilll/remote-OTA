# 命令与检查清单

以下命令用于复核冻结基线和下一阶段取证。命令按环境分组；不要在 Windows、Ubuntu SDK 主机和目标板之间混用路径。

## Ubuntu SDK：配置与源码

```bash
cd /home/liu2004/work/Linux_SDK/atk_dlrk3588_linux5.10

grep -E \
'^(RK_DEFCONFIG|RK_AB_UPDATE|RK_PARAMETER|RK_UBOOT_CFG|RK_UBOOT_CFG_FRAGMENTS|RK_BUILDROOT_CFG|RK_ROOTFS_SYSTEM|RK_ROOTFS_TYPE)=' \
output/.config

./build.sh print-parts

cat u-boot/configs/alientek_rk3588-ab.config

grep -E \
'^(CONFIG_ANDROID_AB|CONFIG_SPL_AB|CONFIG_AVB_LIBAVB_AB|CONFIG_ROCKCHIP_FIT_IMAGE)=' \
u-boot/.config

grep '^# CONFIG_SPL_KERNEL_BOOT is not set$' u-boot/.config

grep -RIn \
  -e 'bootargs_add_android' \
  -e 'ab_update_root_partition' \
  -e 'ab_update_root_uuid' \
  -e 'ANDROID_PARTITION_SYSTEM' \
  -e 'rk_avb_append_part_slot' \
  u-boot/common \
  u-boot/arch/arm/mach-rockchip \
  u-boot/disk \
  u-boot/include \
  u-boot/lib 2>/dev/null
```

## Ubuntu SDK：产物复核

```bash
find output/firmware -maxdepth 2 \
  \( -type f -o -type l \) \
  -printf '%p -> %l  %s bytes\n' 2>/dev/null | sort

readlink -f output/firmware/parameter.txt
cat output/firmware/parameter.txt

readlink -f output/firmware/update.img

cat output/update-ab/Image/package-file

ls -lhL \
  output/update-ab/Image/MiniLoaderAll.bin \
  output/update-ab/Image/parameter.txt \
  output/update-ab/Image/uboot.img \
  output/update-ab/Image/boot.img \
  output/update-ab/Image/rootfs.img \
  output/update-ab/Image/update.img

sha256sum \
  output/update-ab/Image/MiniLoaderAll.bin \
  output/update-ab/Image/parameter.txt \
  output/update-ab/Image/uboot.img \
  output/update-ab/Image/boot.img \
  output/update-ab/Image/rootfs.img \
  output/update-ab/Image/update.img
```

如果 `package-file` 路径不同，先查找：

```bash
find output -type f -name package-file -print
```

## 目标板：只读基线

```bash
edgeguard-rk-abctl status

cat /proc/cmdline

awk '$5 == "/" {print}' /proc/self/mountinfo

cat /proc/partitions

ls -l /dev/disk/by-partlabel
ls -l /dev/disk/by-partuuid 2>/dev/null

blkid
```

解析当前 cmdline PARTUUID：

```bash
ROOT_PARTUUID="$(sed -n 's/.*root=PARTUUID=\([^ ]*\).*/\1/p' /proc/cmdline)"
printf 'root PARTUUID=%s\n' "$ROOT_PARTUUID"
readlink -f "/dev/disk/by-partuuid/$ROOT_PARTUUID"
```

冻结基线 A 的预期闭环：

```text
android_slotsufix=_a
root PARTUUID=4b1a0000-0000-4208-8000-4c2e00003e31
PARTUUID -> /dev/mmcblk0p6
/ mount = 179:6
/dev/mmcblk0p6 -> system_a
```

B 的预期闭环：

```text
android_slotsufix=_b
root PARTUUID=c3180000-0000-4d5b-8000-5dbf00004e66
PARTUUID -> /dev/mmcblk0p7
/ mount = 179:7
/dev/mmcblk0p7 -> system_b
```

PARTUUID 会在重新生成 GPT 后变化，所以后续应以当时 `/dev/disk/by-partuuid` 为准，不把上述 GUID 当成永久产品常量。

## 目标板：Native A/B 查询

```bash
edgeguard-rk-abctl status
edgeguard-rk-abctl get-current
edgeguard-rk-abctl get-primary
```

预期每个查询都应有严格、可供程序解析的 stdout 和可靠 exit code。RAUC 接入前把 stdout、stderr、`$?` 一并记录。

## RAUC 下一阶段的只读检查

RAUC 部署后先执行：

```bash
rauc --version
rauc status
rauc status --detailed 2>/dev/null || true
```

再把 RAUC 的 booted/primary/good-bad 结果与以下命令逐项比对：

```bash
edgeguard-rk-abctl status
cat /proc/cmdline
awk '$5 == "/" {print}' /proc/self/mountinfo
```

## 受控写操作

下列命令会改变下一次启动或槽状态，只能在 A/B 两组镜像已知可启动、UART 和恢复前提具备、当前证据已保存时运行：

```bash
edgeguard-rk-abctl set-primary b
reboot

edgeguard-rk-abctl mark-good current
```

RAUC 集成后，优先通过 RAUC 的正式命令触发状态转换；不要同时让人工 `edgeguard-rk-abctl` 与 RAUC 并发写 metadata。

真实安装命令的 bundle 路径和 RAUC options 必须来自已验签的当次 artifact，不在冻结文档中硬编码。执行安装前至少保存：

```bash
date -u
rauc --version
rauc status
edgeguard-rk-abctl status
cat /proc/cmdline
awk '$5 == "/" {print}' /proc/self/mountinfo
```

## 每轮验证后的最小记录

```text
timestamp
device/board identity
software/build identity
command and exit code
stdout/stderr
metadata before and after
cmdline
root mount identity
UART log reference
artifact hash
resulting evidence level
```

只有真实观察所在层的证据，才能使用相应的 `SOURCE VERIFIED`、`SDK CONFIG VERIFIED`、`SDK BUILD VERIFIED`、`OFFLINE ARTIFACT VERIFIED`、`TARGET VERIFIED` 或 `HARDWARE VERIFIED` 标签。

