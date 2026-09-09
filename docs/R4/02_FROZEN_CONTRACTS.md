# R4 冻结 Contracts

## 1. 冻结架构

```text
Buildroot final image
  -> signed RAUC bundle (rootfs + boot)
  -> immutable Server release + manifest
  -> long-running Agent autonomous poll
  -> durable download / verify / install orchestration
  -> RAUC installs inactive slot group
  -> custom backend updates Native Rockchip A/B metadata
  -> controlled reboot
  -> candidate identity + expected slot + local health
  -> guarded mark-good OR guarded rollback
  -> durable report obligation
  -> IDLE and later autonomous polling
```

R5/R6 只能在这些边界上增量扩展，不得把 R4 当作待重写 prototype。

## 2. 组件所有权

| 组件 | 唯一职责 | 禁止侵入 |
|---|---|---|
| OTA Agent | discovery、download、verification、install orchestration、reboot recovery、health outcome orchestration、reporting | 不直接写 block/misc/AvbABData；不拥有 Wi-Fi 密码 |
| Health hook | 有界、只读、本地 firmware/control-plane readiness | 不连接 AP、不改配置、不 mark、不 reboot |
| RAUC 1.5.1 | bundle 验签、identity 验证、inactive slot group 安装 | Agent 不绕过 RAUC 写镜像 |
| RAUC custom backend / pre-install | 将 RAUC boot-control 语义映射到既有 Native A/B，并阻止写当前槽 | 不改变 AvbABData ABI |
| `edgeguard-rk-abctl` | 唯一 Native A/B control surface | 上层不复制 retry counter 或 raw metadata writer |
| U-Boot/SPL | pending/retry/fallback/boot selection | Agent 不重实现 bootloader retry |
| Server | manifest、artifact/Range、report acceptance/log correlation | 不决定 target slot，不拥有设备 durable state |
| `/userdata` | 跨 rootfs/slot 保存 OTA attempt、bundle partial/status | slot rootfs 不作为跨升级 runtime credential store |
| Provisioning（后续） | Wi-Fi credential、可持久 runtime config、可能的 endpoint provisioning | 默认不并入 OTA Agent 状态机 |

## 3. 状态机 ABI

状态名与 schema v1 含义冻结：

```text
IDLE
-> CHECK_NETWORK
-> CHECK_UPDATE
-> PRECHECK
-> DOWNLOADING
-> VERIFY_DOWNLOAD
-> RAUC_VERIFY
-> INSTALLING
-> REBOOT_PENDING

reboot

BOOT_NEW_SLOT
-> HEALTH_CHECK
   -> MARK_GOOD -> REPORT_SUCCESS -> IDLE
   -> guarded mark-bad -> ROLLBACK -> reboot/fallback recovery

terminal/diagnostic path: ERROR
```

冻结要求：

- 不增加、重命名、重编号或重新解释 durable state，除非单独设计兼容迁移；
- `attempt_id`、target version/build、previous/expected slot、boot ID/reboot context 与 root error 在 attempt 结束前保持 correlation；
- `INSTALLING` 是 destructive/uncertain boundary，不是普通 retry state；
- `REPORT_SUCCESS` 是 mark-good 后的外部交付债务，不是 firmware failure；
- process restart、rootfs reboot 和 server outage 不允许抹除未完成 obligation。

## 4. 不可回退安全不变量

1. 同一 `attempt_id` 的 destructive RAUC install 调用次数必须 `<= 1`。
2. install timeout/CLI lost/结果不确定时，不得自动 reinstall 同一 attempt。
3. destructive action 前必须先完成对应 durable intent；持久化失败则 fail closed。
4. mark-good 必须同时满足：booted slot 是 expected candidate、release identity 全匹配、built-in health 与外部 hook 成功。
5. health failure 不得经过 mark-good；mark-bad 只能作用于预期当前 candidate，且不能让两槽都不可启动。
6. mark-good 已提交后，report failure 不得触发 reinstall、re-health、重复 mark-good 或 rollback。
7. Server/AP/DHCP/DNS/Internet outage 本身不是 firmware health failure。
8. Agent、hook、S99 不得直接写 `/dev/mmcblk*`、`misc` 或 AvbABData raw storage。
9. S99 只负责验证持久目录/marker 和 Agent lifecycle，不成为第二状态机。
10. `.part` 只可在精确 `206 Content-Range` 匹配时 append；Range 被忽略为 `200` 时必须从零安全重下。
11. release identity 链任一 compatible/version/build/size/SHA 不一致都 fail closed。
12. 已接受 candidate 与 report delivery 分两次 commit；二者不能回卷。

## 5. Wire/API contract

### Manifest

```json
{
  "schema_version": 1,
  "device_compatible": "atk-dlrk3588",
  "version": "1.2.5",
  "build_id": "rk3588-r4-1.2.5-001",
  "artifact_url": "/update.raucb",
  "sha256": "<64 lowercase hex>",
  "size": 586181970,
  "mandatory": false
}
```

`artifact_url` 保持 relative；endpoint/address 不是 release identity 的组成部分。

### Artifact

- endpoint 保持 `GET /update.raucb`；
- active artifact 与启动时缓存 metadata 一致且 immutable；
- 支持 full response 和单一 byte Range；
- 不引入多 Range、delta OTA 或另一个 artifact protocol。

### Device report

endpoint 保持 `POST /device/report`。Legacy required fields 保持：

```text
device_id, current_version, status, timestamp
```

R4 additive optional fields：

```text
attempt_id, target_version, build_id,
agent_state, error_code, timestamp_valid, uptime_ms
```

Legacy payload 必须继续返回成功；optional fields 严格类型验证；无关 unknown fields 继续拒绝；不得新建第二 report endpoint。

## 6. Health contract

Firmware acceptance 由以下组合构成：

```text
candidate release identity
+ expected current slot
+ writable/persistent /userdata
+ RAUC and boot-control runtime
+ local Wi-Fi control-plane hook
```

Wi-Fi hook 的 PASS 只说明固件内本地组件存在并可被控制面枚举。以下全部不是 R4 firmware-health hard gate：

```text
saved AP profile
Wi-Fi credential
association
DHCP/address
DNS
Internet
OTA Server/report endpoint reachability
```

## 7. Evidence level contract

| 等级 | 允许的依据 | 不允许扩大的结论 |
|---|---|---|
| `CODE REVIEWED` | 仓库 source/static inspection | runtime 成功 |
| `HOST VERIFIED` | host/VM 自动测试与 fixture | SDK image 或真机行为 |
| `SDK BUILD VERIFIED` | vendor SDK 完整构建、最终镜像与 bundle 检查 | 真机 boot/install/rollback |
| `TARGET OBSERVED` | 真实 target 单点状态 | 完整 campaign |
| `TARGET VERIFIED` | 有边界、有 correlation、有保留证据的真实 target campaign | 未执行的场景或 fleet 泛化 |
| `PROJECT ACCEPTED` | 阶段负责人按书面 criteria 签收 | 自动补齐缺失 evidence |

任何后续报告必须保留 achieved level，不得把 Codex 输出、host 测试或 SDK build 写成 target evidence。

