# RAUC 下一阶段交接

## 任务边界

下一阶段只把 RAUC 接到已验证的 Rockchip Native A/B 平台。不要重做 bootloader、GPT 或 metadata 方案。

```text
Input baseline:  Native A/B platform = TARGET VERIFIED
Work stage:      BG-RAUC-BOOTCONTROL
Goal:            close RAUC custom backend and end-to-end lifecycle evidence
```

## 开始前必须重新确认

1. 读取设备当前 `edgeguard-rk-abctl status`，与冻结稳定状态比对。
2. 确认 suffix、root PARTUUID、实际根挂载一致。
3. 确认目标 rootfs 的 RAUC 精确版本、build options 和 custom backend ABI；不要按别的版本文档假设。
4. 确认签名/keyring、bundle compatible、slot image 类型和健康检查策略。
5. 若总门要求 recovery，先补 Loader/Maskrom 的 `HARDWARE VERIFIED` 证据。

## 推荐的 system.conf 方向

下面是设计起点，不是已经部署的配置：

```ini
[system]
compatible=EdgeGuard-ATK-DLRK3588-RK3588
bootloader=custom

[handlers]
bootloader-custom-backend=/usr/libexec/rauc/edgeguard-rk-abctl

[slot.rootfs.0]
device=/dev/disk/by-partlabel/system_a
type=ext4
bootname=a

[slot.boot.0]
device=/dev/disk/by-partlabel/boot_a
type=raw
parent=rootfs.0

[slot.rootfs.1]
device=/dev/disk/by-partlabel/system_b
type=ext4
bootname=b

[slot.boot.1]
device=/dev/disk/by-partlabel/boot_b
type=raw
parent=rootfs.1
```

设计意图：`system_a/system_b` 是各启动组的 bootable parent，`boot_a/boot_b` 是同组 child。使用 stable partlabel 路径，避免分区号耦合。

在采用前必须对照目标 RAUC 版本确认：custom backend 配置键、parent/group 语义、raw boot image handler、bootname 值和 booted-slot detection。

## Backend ABI 入口

按目标 RAUC 版本逐项确认并实现：

```text
get-primary
set-primary <bootname>
get-state <bootname>
set-state <bootname> <good|bad>
get-current  # 版本支持或要求时
```

约束：

- stdout 只能输出 ABI 规定值；诊断写 stderr；
- 成功/失败 exit code 必须准确；
- 每个写操作都要 write-back read verification；
- current 检测以 suffix + PARTUUID/root mount 双重检查为准；
- Native pending/good/bad 到 RAUC good/bad 的映射必须通过该 RAUC 版本的生命周期测试，不凭名称猜；
- 不允许把从未启动的 inactive slot 直接标为 successful-good。

## 建议实施顺序

1. **版本与 ABI 冻结**：记录 `rauc --version`、system.conf schema、backend 调用约定。
2. **只读 backend**：先实现/验证 `get-current`、`get-primary`、`get-state`，在 A/B 两槽分别比对 Native 状态。
3. **写操作单测**：在受控 metadata 副本或专用测试镜像上验证 CRC、短写失败、非法参数、幂等和状态变换。
4. **目标板非安装测试**：用 RAUC 的 status/mark 命令确认 backend 调用、stdout 和 exit code。
5. **slot topology 检查**：确保当前组受保护、inactive rootfs 与 boot child 同组。
6. **签名 bundle 离线验收**：确认 compatible、manifest、boot/rootfs image、尺寸、签名和 keyring。
7. **首次 RAUC 安装**：只安装 inactive group；保留 UART 和 Native metadata 前后快照。
8. **切换与确认**：重启后证明 boot suffix、root PARTUUID、实际根挂载均为新槽；健康检查通过后才 mark-good。
9. **confirmed 稳定性**：多次普通重启确认 retries 不再下降。
10. **受控回退**：安装/选择一个可控失败候选，不 mark-good，证明 retries 递减和自动回退；随后恢复双槽 good。

## 每个阶段应保存的证据

```text
RAUC version and build config
system.conf
backend executable hash and source revision
backend verb transcript with exit codes
rauc status before/after
bundle manifest, signature verification and SHA-256
Native metadata before install, before reboot, after boot, after mark-good
/proc/cmdline
/proc/self/mountinfo root entry
/dev/disk/by-partlabel and by-partuuid mappings
UART boot log
rollback retry sequence
final recovered stable state
```

## 失败即停止的条件

- RAUC 认为的 current 与 Native current 不一致；
- suffix、root PARTUUID、根挂载不一致；
- RAUC 尝试写当前 active slot；
- boot 与 rootfs child/parent 不在同一 inactive group；
- backend 输出格式或 exit code 不符合 ABI；
- metadata CRC/读回验证失败；
- 新槽还未健康验证却被标记 successful-good；
- recovery 前提不满足且下一步可能使设备不可启动。

这些问题必须先修复并重新建立证据，不能通过重试自动安装绕过。

