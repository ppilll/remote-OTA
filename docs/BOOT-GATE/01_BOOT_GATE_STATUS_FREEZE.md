# BOOT-GATE 状态冻结

## 规范状态

```yaml
freeze_date: 2026-09-04
platform: ATK-DLRK3588 / RK3588 / eMMC / Buildroot

rockchip_native_ab_platform_baseline: TARGET_VERIFIED
bg_native_ab_control: CLOSED

boot_gate:
  state: HELD_AT_PRE_RAUC_BOUNDARY
  closed: false
  hold_reason: RAUC custom bootloader backend and RAUC end-to-end lifecycle are not verified

rauc_integration:
  state: NOT_STARTED_BY_DESIGN

recovery_path:
  loader_or_maskrom_detection: NOT_EVIDENCED_IN_THIS_PACKET
  recovery_reflash: NOT_EVIDENCED_IN_THIS_PACKET
  evidence_level: NOT_HARDWARE_VERIFIED
```

## 已关闭的子门

`BG-NATIVE-AB-CONTROL` 已关闭，依据是真实目标板上完成了：

- `misc + 2048` 的 Rockchip `AvbABData` 可读，32 字节、version 1.0、CRC 正确；
- 当前槽、primary 和 `last_boot` 可读；
- A、B 均能被设为 primary 并实际启动；
- `boot_a + system_a` 与 `boot_b + system_b` 选择一致；
- 当前槽 mark-good 后变为 `successful=1, tries=0`；
- 未确认槽每次启动消耗一次 retry；
- retries 耗尽后槽变为 canonical bad，bootloader 自动选择另一 confirmed-good 槽；
- bad 槽可通过 set-primary 重新变为 pending/bootable，启动确认后恢复为 good；
- 最终稳定状态已恢复。

## 最终稳定基线

```text
current=a
primary=a
last_boot=a

slot a: priority=15 tries=0 successful=1 is_update=0 bootable=yes
slot b: priority=14 tries=0 successful=1 is_update=0 bootable=yes
```

这个状态是后续 RAUC 集成开始前应重新采集并比对的基线。若实际设备已被后来操作改变，应记录新状态，不能假定仍与冻结时一致。

## 为什么总 BOOT-GATE 不能关闭

Native A/B 只证明 bootloader、分区、metadata 和最小 userspace 控制面工作。目标架构中的生产路径是：

```text
Remote OTA Agent
  -> RAUC bundle verification and installation
  -> inactive slot group
  -> RAUC custom bootloader backend
  -> Rockchip AvbABData
  -> U-Boot/SPL selection
  -> booted system health confirmation
  -> mark-good or automatic rollback
```

目前从 RAUC 开始的这段链路没有实测。因此只能冻结在 pre-RAUC 边界。

## BOOT-GATE 总门的后续退出条件

下列条件全部成立后，才能提出 `BOOT-GATE = CLOSED`：

1. 确认目标 rootfs 中实际 RAUC 版本及其 custom backend ABI。
2. backend 的 `get-primary`、`set-primary`、`get-state`、`set-state`，以及该版本需要/支持时的 `get-current` 行为通过测试。
3. RAUC 正确识别当前 booted slot，且 suffix、root PARTUUID、实际根挂载三者不一致时必须失败。
4. RAUC slot topology 把每个 rootfs 与同组 boot 分区绑定，并只写 inactive group。
5. 真实签名 bundle 安装到 inactive group 后，RAUC 触发正确的 next-boot 选择。
6. 新槽启动后先处于 pending，再经健康检查 mark-good；确认后的普通重启不继续消耗 retries。
7. 受控失败场景真实证明 retry 递减、目标槽不可启动和自动回退。
8. 失败后 RAUC 状态、Native metadata、当前 rootfs 身份彼此一致。
9. 如果项目总退出条件包含灾难恢复，Loader/Maskrom 进入和恢复刷写须达到 `HARDWARE VERIFIED`。

