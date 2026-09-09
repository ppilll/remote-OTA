# R4 最终基线

## 1. 冻结身份

| 项目 | 冻结值 |
|---|---|
| Project | EdgeGuard Remote OTA |
| Board | 正点原子 ATK-DLRK3588 |
| SoC / ISA | Rockchip RK3588 / AArch64 |
| Build system | Rockchip/正点原子 vendor Buildroot 2021.11 family |
| Kernel | Linux 5.10.209 family |
| U-Boot | Rockchip vendor U-Boot 2017.09 family |
| OTA installer | RAUC 1.5.1 |
| Boot control | RAUC custom backend → `edgeguard-rk-abctl` → Rockchip `AvbABData` |
| Persistent OTA state | `/userdata/edgeguard-remote-ota` |
| Target release after R4 | `1.2.5` |
| Target build ID after R4 | `rk3588-r4-1.2.5-001` |

## 2. 当前真实 target 基线

R4 happy direction 的保留 target correlation：

```text
attempt_id              = 48f3bd95-70b0-4e12-b44c-7d593aada87a
source release          = 1.2.4 / rk3588-r3h-1.2.4-001
source slot             = b
candidate release       = 1.2.5 / rk3588-r4-1.2.5-001
candidate slot          = a
boot_id_before          = c0b19120-7518-4887-8b53-52110850010e
boot_id_after           = 1f9eea0b-105d-4f20-973f-d24c11881dc2
bundle size             = 586181970
bundle sha256           = b88a407068b4022c21e8909a8e76257f58fec08550b3e12db8f4fdefd26e5583
```

最终 target snapshot：

```text
release                 = 1.2.5 / rk3588-r4-1.2.5-001
rootfs                  = /dev/mmcblk0p6
current                 = a
primary                 = a
last_boot               = a
slot a                  = priority=15 tries=0 successful=1 confirmed-good
slot b                  = priority=14 tries=0 successful=1 confirmed-good
Agent                   = running (observed PID 862 after reboot)
durable state           = REPORT_SUCCESS, then IDLE after report delivery
last_error              = NONE
```

RAUC slot mapping保持：

```text
rootfs.0 / boot.0 = system_a / boot_a = bootname a
rootfs.1 / boot.1 = system_b / boot_b = bootname b
```

上面是 R4 结束时的交接起点。R5/R6 开始前仍应只读确认实际设备没有被之后的实验改变；不得通过写 slot metadata 或删除 durable state 来“恢复”本表。

## 3. Server 与网络基线

R4 最终 campaign 使用的实验室链路：

```text
Server = http://192.168.77.1:8000
Target = 192.168.77.2
manifest path = /manifest.json
artifact path = /update.raucb
report path = /device/report
```

Server contract：

- `GET /manifest.json` 返回单一 active release 的 schema v1 metadata；
- `GET /update.raucb` 支持 full body、单 Range、正确 `206 Content-Range` 和 `416`；
- `POST /device/report` 保持 legacy payload 兼容，并可接收 Agent 已实现的 extended optional fields；
- active bundle 对其 version/build identity 必须 immutable；
- Server 启动时缓存 active manifest，发布新 candidate 后必须按冻结 publish 流程完成原子替换、重启/重新加载和 HTTP byte closure。

仓库最新 `main` 中 `buildroot-external/package/edgeguard-remote-ota/agent.conf` 与 R4 campaign 已统一为 `http://192.168.77.1:8000`。早期 R4 planning documents 中的 `http://10.119.65.50:8000` 是历史实验室快照，不能再作为当前 baseline。后续阶段不得静默硬编码第三个地址；产品化 persistent endpoint provisioning 属于后续配置工作。

## 4. SDK 与 release artifact 基线

R4 使用现有、不可重命名的构建/发布入口：

```text
build-r3-candidate.sh
publish-r3-candidate.sh
```

冻结 build identity：

```text
VERSION  = 1.2.5
BUILD_ID = rk3588-r4-1.2.5-001
PROFILE  = all
```

保留的 R4 build record 支持以下结论：

- vendor Buildroot 完整 build 成功；
- final staged target checker 与 release identity checker 成功；
- final rootfs identity 与 R4 Agent/config/hook 集成成功；
- RAUC production profile 保持 1.5.1/custom backend/pre-install handler；
- signed rootfs+boot RAUC bundle 生成并验证成功；
- exact bundle 经过 publish 和 HTTP size/SHA/byte closure。

此候选可标记 `SDK BUILD VERIFIED`。该标签只覆盖对应 SDK build/artifact，不替代 target lifecycle evidence。

每个后续 candidate 必须：

```text
VERSION monotonically increasing
BUILD_ID unique
same VERSION + BUILD_ID => immutable bytes
rootfs release.json == server manifest == signed RAUC manifest == post-boot release.json
```

## 5. RAUC production profile

冻结配置语义：

```ini
[system]
compatible=EdgeGuard-ATK-DLRK3588-RK3588
bootloader=custom
statusfile=/userdata/edgeguard-remote-ota/rauc-status.raucs

[keyring]
path=/etc/rauc/keyring.pem
check-purpose=codesign

[handlers]
bootloader-custom-backend=/usr/libexec/rauc/edgeguard-rk-ab-backend
pre-install=/usr/libexec/rauc/edgeguard-rk-ab-preinstall
```

Production A/B 保持对称可写，不加入 `readonly=true`，不升级/现代化 RAUC 版本语义。

## 6. Agent 基线

关键路径与值：

```text
binary            = /usr/bin/edgeguard-remote-ota-agent
service           = /etc/init.d/S99edgeguard-remote-ota
state_dir         = /userdata/edgeguard-remote-ota
state_file        = /userdata/edgeguard-remote-ota/agent-state.json
part_file         = /userdata/edgeguard-remote-ota/update.raucb.part
bundle_file       = /userdata/edgeguard-remote-ota/update.raucb
release_file      = /etc/edgeguard-ota/release.json
health hook       = /usr/libexec/edgeguard/edgeguard-health-check
health timeout    = 30 seconds
reporting mode    = extended for R4 candidate
poll interval     = 300 seconds
```

Health hook 的最终冻结实现为最多 5 次本地只读 sample、间隔 5 秒、连续 3 次成功即 PASS；它只检查 `wlan0`、`connmand`、`wpa_supplicant` 与 ConnMan Wi-Fi technology，不检查 AP、credential、DHCP、DNS、Internet 或 Server reachability。早期 Thread 1 报告中的 6-sample proposed value 已被最终 `main`/SDK/target baseline 取代。

`/userdata/edgeguard-remote-ota/autostart-disabled` 仍是显式维护 marker。正常产品/campaign 启动必须无此 marker；不得删除 attempt state 来代替正确 recovery。
