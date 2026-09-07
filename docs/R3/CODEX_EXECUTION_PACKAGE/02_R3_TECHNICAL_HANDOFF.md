# R3 技术说明与冻结证据

## 1. R3 的职责边界

R3 在既有 Rockchip Native A/B substrate 上增加生产级 Remote OTA 编排，不重新定义 GPT、slot ABI 或 bootloader A/B 语义。

核心链路：

```text
Buildroot final image
  → signed RAUC bundle (rootfs + boot)
  → permanent HTTP server manifest/artifact
  → production Agent durable state machine
  → RAUC install inactive slot
  → Native A/B pending/primary + controlled reboot
  → expected new slot health + mark-good
  → success report + IDLE
```

主要组件：

- Ubuntu 构建/发布：`build-r3-candidate.sh`、`publish-r3-candidate.sh`
- Server：`edgeguard-ota.service`，FastAPI/uvicorn，端口 8000
- Target Agent：`/usr/bin/edgeguard-remote-ota-agent`
- Target service：`/etc/init.d/S99edgeguard-remote-ota`
- RAUC：1.5.1，custom bootloader backend + pre-install handler
- Native A/B 状态：`edgeguard-rk-abctl`
- 跨槽持久数据：`/userdata/edgeguard-remote-ota`

production RAUC profile 冻结为：

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

A/B 为对称可写；production profile 不含 `readonly=true`。

## 2. Agent 状态机

升级前半程：

```text
IDLE → CHECK_NETWORK → CHECK_UPDATE → PRECHECK → DOWNLOADING
→ VERIFY_DOWNLOAD → RAUC_VERIFY → INSTALLING → REBOOT_PENDING
```

重启后：

```text
REBOOT_PENDING → BOOT_NEW_SLOT → HEALTH_CHECK → MARK_GOOD
→ REPORT_SUCCESS → IDLE
```

状态、attempt identity、目标 identity、预期槽、前一槽和 reboot context 均写入 `/userdata/.../agent-state.json`，用于跨进程/跨重启恢复。

## 3. 关键安全不变量

1. 身份链必须完全一致：Buildroot `release.json` → HTTP manifest → signed RAUC manifest → reboot 后 `/etc/edgeguard-ota/release.json`。
2. 任一 version/build/compatible/size/SHA 不一致都 fail closed。
3. 修改进入 signed image 的配置或代码后，必须使用新的 `VERSION/BUILD_ID` 重建；禁止复用 identity。
4. 正常升级中只有 Agent 可以下载、验签、调用 RAUC install、触发 reboot、mark-good 和上报。
5. RAUC 只写 inactive rootfs/boot pair；不在 R3 层绕过或重构 Native A/B。
6. 同一 `attempt_id` 的 install 不可重入；timeout/结果不确定时绝不 reinstall。
7. 只有 boot 到 `expected_candidate_slot`、release identity 正确且 health 成功后才能 mark-good。
8. mark-good 成功后，上报失败只能重试 reporting；不能第二次 mark-good、reinstall 或 rollback。
9. 不通过删除 durable state、杀 RAUC、强制写 slot 或手工补 mark-good 来制造成功证据。
10. `/userdata` 未正确挂载或不可写时，S99/Agent 应 fail closed。

## 4. autostart suppression 机制

R3-H 为 foreground campaign 增加共享 marker：

```text
/userdata/edgeguard-remote-ota/autostart-disabled
```

marker 存在时，S99 返回成功但不启动 Agent；直接前台执行 Agent binary 不受影响。该机制跨 A/B 槽生效，不修改 Native A/B。正式日常升级应保持 marker 不存在。

冻结证据显示 marker suppression 已在真实 RK3588 上生效，state 未被 S99 改动；完成 1.2.4 campaign 后 marker 已移为 `.saved`，S99 成功启动 Agent并完成 mark-good。

## 5. 已验证项

### Host / SDK（已冻结，不重复）

- Ubuntu core：8/8 PASS。
- Ubuntu orchestration：5/5 PASS。
- Thread 2、Thread 3 C suites：全部 PASS。
- merged integration：36/36 PASS，merged RC=0。
- production compile：GNU11、`-Wall -Wextra -Werror`，full link PASS；production service binder 为 strong `ota_agent_services_init`。
- Vendor Buildroot olddefconfig、AArch64 package cross compile、完整 Buildroot、post-build checker、release target checker：PASS。
- production packages 已启用：json-glib、libcurl、glib2、RAUC、EdgeGuard RK A/B、EdgeGuard Remote OTA。
- final image 的 Agent ELF、动态库、BusyBox/SysV 命令依赖、release/config、production RAUC profile、keyring：PASS。
- build/publish 入口的 candidate bytes、签名 identity、server config、manifest、完整 HTTP bundle size/SHA/byte equality：PASS。
- HTTP Range 0、non-zero Range、416 与 legacy report 204：HOST VERIFIED。

### 真实 RK3588 happy path（已冻结）

- production Agent 经 Wi-Fi 完成 `1.2.3 → 1.2.4` 下载、校验、RAUC install 和自动 controlled reboot。
- attempt：`c035795a-2a2e-4c23-8dce-8457fb91a579`。
- reboot 前：`previous_slot=a`，`expected_candidate_slot=b`，state=`REBOOT_PENDING`。
- boot id 从 `3f670495-8feb-4cde-b914-d6c0fd14d934` 变为 `46a5d342-d7e1-471f-9f7e-7e3fce4d9e36`。
- reboot 后：`current=b`、`primary=b`；B 初始为 Native `pending`。
- candidate release：`1.2.4 / rk3588-r3h-1.2.4-001`。
- 随后 production Agent 继续 recovery，state 到 `REPORT_SUCCESS`，B 变为 `successful=1 / confirmed-good / tries=0`；A 仍为 confirmed-good。
- autostart suppression 与恢复启动行为均在真机验证。

1.2.4 signed bundle 冻结 identity：

```text
compatible = EdgeGuard-ATK-DLRK3588-RK3588
version    = 1.2.4
build      = rk3588-r3h-1.2.4-001
images     = rootfs + boot
size       = 586194258
sha256     = 15a15d44360cd4ae3a78dfee77b4c4bf3193e2e791630d58d073cd59fec53c2a
```

## 6. 未验证/不得声称完成

以下 target-side failure campaigns 尚未形成冻结硬件证据：

- interrupted download / resume
- HTTP 503 retry
- Range fallback 的 target 行为
- signed compatible/version/build mismatch
- downloaded bundle hash mismatch
- health failure、mark-bad、rollback
- report outage after mark-good
- RAUC install CLI timeout / install outcome uncertain
- 上述故障下的跨重启 recovery 与“同 attempt 不重复 install”硬件证明

这些是 R3 后续可靠性工作，不影响当前 happy-path 交付结论，但不能用 host tests 代替 target 结论。

## 7. 给后续 Chat/Codex 的一句话边界

继续工作时以这里的冻结状态为起点：R3 host/SDK 和真实 RK3588 happy path 已完成；不要重跑它们，不要重新设计 Native A/B，不要加入 Git hygiene。下一步若继续验证，应按单一 failure scenario 建立独立 attempt 和证据闭环，最高优先级是 install timeout/outcome uncertain 的 non-reinstall 不变量。

