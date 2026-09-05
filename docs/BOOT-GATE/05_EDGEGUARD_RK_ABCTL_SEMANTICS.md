# `edgeguard-rk-abctl` 语义冻结

## 定位

`edgeguard-rk-abctl` 是 Rockchip Native A/B boot-control 工具及未来 RAUC custom backend 的实现基础。它不是 Remote OTA Agent。

```text
Remote OTA Agent -> RAUC -> edgeguard-rk-abctl / shared library -> misc AvbABData
```

Agent 永远不直接操作 `misc`。

## 已在目标板验证的格式

```text
format=Rockchip-AvbABData
device=/dev/disk/by-partlabel/misc
offset=2048
size=32
version=1.0
crc=ok
```

每个槽至少呈现：

```text
priority
tries_remaining
successful_boot
is_update
flags/reserved
```

顶层状态包含 `last_boot`。实现必须沿用本地 Rockchip vendor layout，不能直接用另一个 AOSP 结构覆盖它。

## 已验证命令

| 命令 | 已验证语义 |
|---|---|
| `status` | 输出格式、CRC、current、primary、last_boot 与两槽完整状态 |
| `get-current` | 返回真实 booted slot；应由 slot suffix 与 root identity 双重判断 |
| `get-primary` | 返回当前 bootloader 首选槽 |
| `set-primary a|b` | 目标槽设为 `priority=15, tries=7, successful=0`；必要时另一优先级降为 14 |
| `mark-good current` | 当前槽设为 `priority=15, tries=0, successful=1`，并更新 `last_boot` |

现有实板记录明确覆盖 `status`、`set-primary` 和 `mark-good current`。`get-current/get-primary` 被列为现有工具能力；RAUC 接入前仍应逐个检查严格 stdout、stderr 和 exit code。

## Canonical 状态变换

```text
set-primary <slot>:
  priority=15
  tries=7
  successful=0
  bootable=yes

mark-good current:
  priority=15
  tries=0
  successful=1
  last_boot=current

mark/set bad <slot>:
  priority=0
  tries=0
  successful=0
  bootable=no
```

## Current slot 的强校验

current 检测应同时验证：

```text
android_slotsufix=_a/_b
AND
root=PARTUUID=... -> /dev/mmcblk0pN -> system_a/system_b
```

两者不一致、字段缺失、PARTUUID 无法解析或根挂载身份冲突时，返回非零并报告错误。不得猜测。

## 写入要求

- 校验 magic、version、CRC；
- 只读写固定 32 字节窗口；
- 使用明确的 byte order 和 vendor bitfield/layout；
- 写前检查参数和状态转换；
- 写后同步并重新读取验证；
- CRC 覆盖范围与 vendor 源码一致；
- 任何短读、短写、fsync 或验证失败必须返回非零；
- 不把 `dd seek=...` 作为产品实现；
- 不允许并发写者无锁修改同一 metadata。

## RAUC 仍需补齐的 verbs

下一阶段至少要对照目标 RAUC 版本实现并验证：

```text
get-primary
set-primary <bootname>
get-state <bootname>
set-state <bootname> <good|bad>
get-current        # 仅在目标 RAUC 版本支持/需要时
```

Native 是三态，RAUC custom API 通常只暴露 `good/bad`。前一阶段提出的候选映射是：

```text
successful_boot == 1 -> RAUC good
successful_boot == 0 -> RAUC bad/unconfirmed
```

该映射目前是 **RAUC 集成设计候选**，不是 `TARGET VERIFIED`。必须用目标 RAUC 版本的正式接口行为和安装生命周期测试确认，尤其要验证 pending 槽不会被 RAUC 错误地永久置为 unbootable。

安全约束候选：

- `set-state <slot> good` 默认只允许 `slot == current`，防止未启动的 inactive 槽被宣称成功；
- `set-state <slot> bad` 必须允许 inactive 槽，以支持安装前隔离；
- `set-primary` 只负责 next-boot pending 状态，不能顺便宣称 health success。

这些约束也属于下一阶段待验证设计。

