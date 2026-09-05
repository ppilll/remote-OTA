# 目标板 Native A/B 证据

## 初始 A 槽与 rootfs 一致性

目标 cmdline：

```text
android_slotsufix=_a
root=PARTUUID=4b1a0000-0000-4208-8000-4c2e00003e31
```

设备映射与挂载：

```text
4b1a0000-0000-4208-8000-4c2e00003e31 -> /dev/mmcblk0p6
/dev/mmcblk0p6 -> PARTLABEL system_a
/ mount major:minor = 179:6
```

结论：A boot suffix、A system PARTUUID 与实际根挂载一致，`TARGET VERIFIED`。

## Metadata 初始状态和 A mark-good

初次读取：

```text
format=Rockchip-AvbABData offset=2048 size=32 version=1.0 crc=ok
current=a primary=a last_boot=a
slot=a priority=15 tries=6 successful=0 is_update=0 bootable=yes flags=0x00
slot=b priority=14 tries=7 successful=0 is_update=0 bootable=yes flags=0x00
```

A mark-good 后并在重启后保持：

```text
current=a primary=a last_boot=a
slot=a priority=15 tries=0 successful=1 is_update=0 bootable=yes flags=0x00
slot=b priority=14 tries=7 successful=0 is_update=0 bootable=yes flags=0x00
```

这证明 current A 从 pending 转为 confirmed-good，且确认状态跨重启持久化。

## A -> B 切换

在 A 上执行 set-primary B 后：

```text
current=a primary=b last_boot=a
slot=a priority=14 tries=0 successful=1 is_update=0 bootable=yes flags=0x00
slot=b priority=15 tries=7 successful=0 is_update=0 bootable=yes flags=0x00
```

重启进入 B 后：

```text
android_slotsufix=_b
root=PARTUUID=c3180000-0000-4d5b-8000-5dbf00004e66
current=b primary=b last_boot=a
slot=b priority=15 tries=6 successful=0 bootable=yes
/ mount major:minor = 179:7
c3180000-0000-4d5b-8000-5dbf00004e66 -> /dev/mmcblk0p7 -> system_b
```

这同时证明：

- B 被选为 next boot；
- 第一次 pending B 启动消耗一次 retry（7 -> 6）；
- `boot_b + system_b` 选择一致；
- B 的 root PARTUUID 与实际根挂载一致。

B mark-good 后：

```text
current=b primary=b last_boot=b
slot=a priority=14 tries=0 successful=1 bootable=yes
slot=b priority=15 tries=0 successful=1 bootable=yes
```

之后重复多次 A/B 切换和 mark-good，行为一致。

## Retry 耗尽与自动回退

把 B 再次设为 primary 会把它置为 pending：

```text
slot=b priority=15 tries=7 successful=0 bootable=yes
```

不执行 mark-good，连续重启观察到：

```text
tries=6
tries=5
...
tries=2
...
tries=0
```

耗尽后系统自动启动 A：

```text
current=a primary=a last_boot=b
android_slotsufix=_a
root=PARTUUID=4b1a0000-0000-4208-8000-4c2e00003e31
/ = 179:6 = /dev/mmcblk0p6 = system_a
```

B 最终变为 canonical bad：

```text
slot=b priority=0 tries=0 successful=0 is_update=0 bootable=no flags=0x00
```

结论：retry decrement、unconfirmed-slot exhaustion、canonical bad 和自动 B -> A fallback 均为 `TARGET VERIFIED`。

## Bad 槽恢复

在 A 上对 bad B 执行 set-primary：

```text
current=a primary=b last_boot=b
slot=a priority=14 tries=0 successful=1 bootable=yes
slot=b priority=15 tries=7 successful=0 bootable=yes
```

B 随后可启动并 mark-good。之后对 A 执行 set-primary，A 也从 pending 正常启动并 mark-good。这证明 bad 槽可恢复为 pending，再经真实启动恢复为 confirmed-good。

## 冻结时最终状态

```text
format=Rockchip-AvbABData offset=2048 size=32 version=1.0 crc=ok
current=a primary=a last_boot=a
slot=a priority=15 tries=0 successful=1 is_update=0 bootable=yes flags=0x00
slot=b priority=14 tries=0 successful=1 is_update=0 bootable=yes flags=0x00
```

## TARGET VERIFIED 清单

- A/B GPT 布局；
- current slot 检测；
- A/B rootfs identity；
- metadata read 与 CRC；
- mark-good；
- set-primary；
- A -> B 与 B -> A；
- pending retry decrement；
- confirmed slot 不消耗 retry；
- retry exhaustion；
- canonical bad；
- automatic fallback；
- bad slot recovery；
- 最终双槽 good 基线。

