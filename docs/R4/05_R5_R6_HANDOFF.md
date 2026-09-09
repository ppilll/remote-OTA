# R5/R6 Handoff Notes

## 1. 开始前必读

后续阶段先读取：

1. 本冻结包；
2. `docs/R4/03_AUTOMATION_INVARIANTS.md`；
3. `docs/R4/04_HEALTH_POLICY.md` 与 `05_FAILURE_RECOVERY_SPEC.md`；
4. `docs/R3/CODEX_EXECUTION_PACKAGE/02_R3_TECHNICAL_HANDOFF.md`；
5. `docs/BOOT-GATE` 的 Native A/B evidence 与 control semantics。

不要以重新审计已关闭 host tests、重命名 build/publish pipeline 或清理 Git 历史作为 R5/R6 入场工作。

## 2. R5 建议责任：provisioning / persistent runtime config

R5 首要新增事项是把“设备身份/网络配置/endpoint 来源”作为独立产品能力设计，而不是修改 OTA Agent 来保存明文密码。

最低设计问题：

- Wi-Fi credential/profile 是否由 ConnMan persistent storage bind/overlay 到 `/userdata`，还是由独立 provisioning service 重建；
- secret 文件 ownership/mode、at-rest protection、日志脱敏、更新撤销和 factory reset；
- A/B 与 full-rootfs replacement 下的 schema/version migration；
- runtime endpoint 的权威来源、优先级、验证和恢复策略；
- BLE 若用于 provisioning，只传配置/控制/状态，不传完整 RAUC bundle；
- provisioning 失败不能直接等同 firmware health failure。

在该设计冻结前：

- 不把 Wi-Fi 密码写入 release image、manifest、report 或证据日志；
- 不让 OTA Agent 静默复制 `/var/lib/connman`；
- 不增加多个硬编码 fallback Server IP；
- 不改变 R4 hook 的 local-only health truth。

## 3. R6 建议责任：可靠性与生产化扩展

可在独立风险评审后考虑：

- controlled power-cut/watchdog campaign；
- RAUC daemon/CLI outcome uncertainty 的 target fault injection；
- long-duration polling/report backlog 与 storage pressure；
- TLS/PKI、authentication、authorization、key rotation；
- fleet/cloud rollout、rate limiting、observability；
- release retention/rollback policy 与 supply-chain controls。

这些工作不得通过放宽 `same attempt install <= 1`、绕过签名/identity 或把 report outage 变成 rollback 来实现。

## 4. 不可回退/不可重构 contracts

除非有单独架构决议、迁移计划和新 target evidence，R5/R6 不得：

- 替换 Rockchip `AvbABData`、GPT、U-Boot/SPL A/B 语义；
- 将 RAUC 1.5.1 custom backend 改成未经验证的新 backend/新版语义；
- 让 Agent 或 hook 直接写 block/misc/Native A/B raw metadata；
- 在 Agent 中复制 bootloader retry counter；
- 重写 durable state machine 或使 R3/R4 schema v1 state 不可读；
- 让 `INSTALLING` 变成 replay-safe retry；
- 在 mark-good 后因 report/network failure reinstall 或 rollback；
- 让 health hook 执行 Wi-Fi enable/connect、mark/reboot/install；
- 破坏 legacy `POST /device/report` compatibility；
- 修改 relative `/update.raucb` artifact contract 或安全 Range append 规则；
- 对同一 VERSION/BUILD_ID 发布不同 bytes。

## 5. 下一 candidate 规则

当前 release 是：

```text
1.2.5 / rk3588-r4-1.2.5-001
slot a confirmed-good
```

后续 candidate 必须使用更高 version 与唯一 build ID；示例 `1.2.6` 只在尚未被其他实验占用时可用，不能仅因文档举例就默认可重用。

构建与发布继续通过：

```text
build-r3-candidate.sh <version> <build_id> all
publish-r3-candidate.sh <version> <build_id>
```

在 R5/R6 修改进入 signed image 的任何 code/config/hook 后，必须新建 identity 并重新完成 SDK build、bundle verify、publish closure；禁止 bundle-only 复用旧 rootfs identity 来掩盖更改。

## 6. 操作边界

正式 target campaign 允许：

- UART/console observation；
- read-only target collection；
- Server/network fault injection；
- SDK build/publish；
- evidence retention。

测量窗口内默认禁止：

```text
restart Agent to trigger OTA
manual rauc install
manual reboot
manual mark-good / mark-bad / set-primary
delete or rewrite agent-state.json
manual bundle download as Agent evidence
manual network repair when claiming unattended network recovery
```

若 provisioning 本身是被测对象，应把允许的 provisioning 输入与开始时点写进独立 campaign plan，不能事后把人工操作改名为自动化。

## 7. 当前已知 handoff 注意项

- 开始前只读确认 target 仍是预期 release/slot/IDLE；近期实验可能已经改变 R4 结束 snapshot。
- 最新 repo `agent.conf` 与 R4 campaign 都使用 `192.168.77.1`；把早期文档中的 `10.119.65.50` 视为历史快照。下一构建仍必须显式决定 endpoint source，不能默认实验室地址就是产品 provisioning。
- Server code deployment 与 candidate publication 必须分离并按冻结顺序执行，防止 active release 被覆盖。
- `REPORT_SUCCESS`、`INSTALLING`、`ROLLBACK` 或带 attempt 的 `ERROR` 不能靠删除 state 恢复。
- 若旧 target snapshot 与本包冲突，以新的真实只读 target evidence 为运行起点，同时保留 R4 frozen evidence 不被改写。
