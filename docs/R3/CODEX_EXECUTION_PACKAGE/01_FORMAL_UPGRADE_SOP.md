# 后续正式升级操作流程

## 0. 固定前提

- Ubuntu 工程：`/home/liu2004/work/EG_OTA`
- 正式构建入口：`build-r3-candidate.sh`
- 正式发布入口：`publish-r3-candidate.sh`
- RK3588 服务：`/etc/init.d/S99edgeguard-remote-ota`
- 生产 Agent：`/usr/bin/edgeguard-remote-ota-agent`
- Agent 配置：`/etc/edgeguard-ota/agent.conf`
- 持久状态目录：`/userdata/edgeguard-remote-ota`
- 当前 OTA LAN：Ubuntu `ens38`，server URL 为 `http://10.119.65.50:8000`
- 当前 `poll_interval_sec=300`；正常运行的 Agent 最迟在下一轮轮询发现发布。

每个正式 candidate 必须使用新的、单调递增的 `VERSION` 和唯一 `BUILD_ID`。同一个 release identity 不得对应两套不同 bytes。

如果 Ubuntu `ens38` 的 DHCP 地址改变，先修改：

```text
/home/liu2004/work/EG_OTA/buildroot-external/package/edgeguard-remote-ota/agent.conf
```

中的 `base_url`，然后使用新的 `VERSION/BUILD_ID` 重新完整 build + publish。不要修改已经签名的 candidate。

## 1. Ubuntu：构建 candidate

示例以下一版 `1.2.5` 为例；实际发布时替换两个变量：

```bash
cd /home/liu2004/work/EG_OTA

VERSION='1.2.5'
BUILD_ID='rk3588-r3h-1.2.5-001'

./build-r3-candidate.sh \
    "$VERSION" \
    "$BUILD_ID" \
    all
```

该入口负责生成最终 rootfs、boot image、签名 RAUC bundle、签名身份信息和 `candidate.env`。不要用裸 `./build.sh buildroot` 代替正式入口。

## 2. Ubuntu：发布 candidate

构建成功后直接发布同一 identity：

```bash
cd /home/liu2004/work/EG_OTA

VERSION='1.2.5'
BUILD_ID='rk3588-r3h-1.2.5-001'

./publish-r3-candidate.sh \
    "$VERSION" \
    "$BUILD_ID"
```

发布脚本会把 bundle 放入 permanent OTA server、更新 manifest 配置并重启 `edgeguard-ota.service`。不要另外重建 bundle，也不要在 server 目录手工替换 bytes。

## 3. RK3588：由生产 Agent 驱动 OTA

正常生产模式下，保持 `/userdata/edgeguard-remote-ota/autostart-disabled` 不存在。发布后有两种启动方式：

### 方式 A：等待自动轮询

无需板端命令。已经运行的 Agent 会在下一次 300 秒轮询内发现新版本，然后自动完成：

```text
manifest → Wi-Fi 下载 → SHA/大小校验 → RAUC 签名/身份校验
→ 写 inactive slot → 设为 pending/primary → 持久化 REBOOT_PENDING
→ 自动 reboot → 新 slot health → mark-good → success report → IDLE
```

### 方式 B：立即触发一轮

在板处于稳定空闲状态时执行：

```sh
/etc/init.d/S99edgeguard-remote-ota restart
```

随后不要再操作。Agent 自己下载 `/update.raucb`、调用一次 RAUC install，并在成功后自动重启。新 slot 启动时 S99 会再次启动 Agent，由 Agent 完成 health、mark-good 和上报。

## 4. 操作红线

- 不要手工 `curl` 下载 `update.raucb`。
- 不要手工执行 `rauc install`、`rauc mark-good` 或 Native A/B 写操作。
- 进入 `INSTALLING` 后不要 Ctrl-C、kill Agent、kill/restart RAUC 或重复启动 Agent。
- 若出现 install timeout 或 outcome uncertain，同一 `attempt_id` 绝不能再次 install；保留 durable state，进入故障处置。
- 不要删除 `agent-state.json` 来“重试”。
- mark-good 已成功但上报失败时，只允许重试 reporting，不得 reinstall、再次 mark-good 或 rollback。

## 5. 前台维护模式（仅需串口观察时）

正式日常升级优先使用 S99。确需前台观察时，在稳定空闲状态且使用可靠串口 console：

```sh
/etc/init.d/S99edgeguard-remote-ota stop
exec /usr/bin/edgeguard-remote-ota-agent \
    --config /etc/edgeguard-ota/agent.conf
```

不要在可能断开的 SSH 会话中承载 `INSTALLING`。如果使用 `autostart-disabled` marker 做前台 campaign，完成后必须有意识地移走 marker，才能恢复新 slot 的 S99 自动启动与自动 mark-good。

