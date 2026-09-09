# R4 问题、根因与决策冻结

## 1. Wi-Fi credential 不跨 full-rootfs replacement

### 现象

设备在旧 rootfs 中已经配置 ConnMan `AutoConnect`，但 OTA 切换到新 rootfs 后仍需要重新输入 Wi-Fi 密码，不能仅依靠旧槽 profile 自动连接。

### 根因

ConnMan learned profile/credential 位于旧槽 rootfs 的 `/var/lib/connman`。A/B 更新写入完整新 rootfs；运行期生成的 profile 没有进入 `/userdata` 等跨槽持久层。`AutoConnect=true` 只对新 rootfs 中已经存在的 favorite service 有效，它不会凭空迁移旧槽 credential。

这不是 RAUC、Native A/B、Agent reboot recovery 或 health hook 失效。

### R4 决策

```text
Wi-Fi credential persistence across full-rootfs replacement = REMOVED FROM R4
automatic reconnection based only on the previous rootfs profile = REMOVED FROM R4
revised R4 acceptance result = N/A, not FAIL/waiver
owner = later provisioning / persistent runtime configuration
```

R4 仍要求 OTA transport 在需要下载/上报时可用，但允许升级后由用户或 provisioning 重新建立网络。失去 AP credential 不得自动判定 candidate firmware unhealthy。

### 后续候选方案

- 将 ConnMan persistent storage 通过受控 bind mount/overlay 迁移到 `/userdata`；或
- 由独立 provisioning service 在 `/userdata` 安全保存所需配置，并在 boot 后重新 provision ConnMan。

默认不要让 OTA Agent 成为 Wi-Fi secret owner。后续设计必须覆盖 secret 权限、日志脱敏、原子更新、factory reset/revoke 和版本迁移。

## 2. Health 与 connectivity 语义混淆

### 风险

若 health hook 依赖 AP、DHCP、DNS 或 Server，会把环境故障误判为坏固件并触发 mark-bad/rollback。

### 解决方案

R4 hook 只读检查 `wlan0`、`connmand`、`wpa_supplicant` 和 ConnMan Wi-Fi technology；最终使用 3 连续成功/5 秒间隔/最多 5 sample/30 秒 Agent budget。AP、credential、DHCP、DNS 和 Server 不参与 mark-good truth。

此责任边界冻结，不得在 R5/R6 为“看起来更自动”而把联网成功重新塞进 firmware health。

## 3. Extended reporting 与 legacy Server 不兼容

### 现象/根因

Agent 已存在 extended report shape，而早期 Server schema 对 extra fields 采用严格拒绝；直接把 candidate 切到 `mode=extended` 会造成持久 `REPORT_SUCCESS` debt。

### 解决方案与当前状态

先向同一 `POST /device/report` additive 扩展 optional fields，并保持 exact legacy payload 兼容；在 Ubuntu host 完成 schema/logging/regression 验证并部署兼容 Server 后，candidate 才使用 `mode=extended`。未新增 endpoint，未改变 target commit ordering。

## 4. Server app deployment 覆盖 active release metadata

### 现象

部署/更新 Server application 后，live release metadata 或 artifact source 可能回到部署包/默认值，破坏已经 publish 的 active candidate identity。

### 根因

`deploy-host-app.sh` 一类 app deployment 操作与 live release publication 共用了目录/配置所有权；部署代码时同时覆盖 active release input。Server 又会在启动时缓存 active manifest，因此文件覆盖和 process state 可形成不一致。

### 解决方案

冻结顺序与所有权：

```text
deploy server code/dependencies/service
-> publish exact immutable candidate
-> atomically replace active artifact/metadata
-> restart/reload server as required
-> verify localhost manifest
-> verify campaign endpoint manifest
-> verify HTTP bundle size/SHA/byte closure
```

最新 `main` 中的 `deploy-host-app.sh` 已实现 release snapshot/injection、部署前后 manifest 等价检查、失败自动 rollback，并明确不触碰 `/opt/edgeguard-ota/artifacts`；release switching 仍只属于 `publish-r3-candidate.sh`。发布 candidate 后不得再用其他 app deployment 覆盖 live release tree。R5/R6 若重做部署布局，必须先显式拆分 application files 与 mutable active-release data；在此之前保持该修复 contract。

## 5. Stale manifest 导致 downgrade rejection

### 现象

实验室准备阶段，Server active manifest 与 target 当前 release 不一致，Agent 进入 `ERROR / DOWNGRADE_REJECTED`，且该预 campaign error 没有正式 attempt identity。

### 根因

Server 发布源/manifest 未恢复到与当时 target baseline 相符的 release，属于实验室 active-release 管理问题，不是 version comparator 绕过。

### 处置与边界

- 恢复正确 Server active manifest；
- 保存 pre-R4 stale-manifest error evidence；
- 仅在 formal campaign 之前，对这个无 `attempt_id` 的实验室状态做一次明确 recovery；
- 此做法不得推广到 `REPORT_SUCCESS`、`INSTALLING`、`ROLLBACK`、带 attempt 的 `ERROR` 或其他 uncertain state。

删除 durable state 不是生产 recovery contract，也不能形成 unattended evidence。

## 6. 早期 planning endpoint 与最终 campaign endpoint 不同

### 现象

早期 R4 文档/本地副本使用 `10.119.65.50:8000`，最终 campaign 与最新 GitHub `main` 的 `agent.conf` 已统一为 `192.168.77.1:8000`。

### 决策

旧地址只作为历史时点信息保留，不能覆盖最终 freeze baseline；当前实验室地址也不能被泛化为 product discovery。R5 的 persistent runtime configuration/provisioning 应决定产品化 endpoint 来源与优先级；R4 不引入 ad-hoc fallback address。

## 7. Post-mark-good 网络/Server unavailable

### 实际结果

新 A slot 已经 health PASS、mark-good 并成为 confirmed-good 时，report 仍因网络/Server 不可用停留在 durable `REPORT_SUCCESS`。网络恢复后同一 obligation 成功交付，state 进入 `IDLE`。

### 冻结结论

```text
firmware acceptance commit != external report delivery
REPORT_SUCCESS retry must not reinstall, re-health, re-mark-good or rollback
```

这是 R4 中必须保留到后续阶段的生产语义。

## 8. Host/环境验证债务

早期 Windows 隔离仓库缺少 FastAPI/Pydantic/编译依赖，部分 pytest/POSIX C suites 只能 compile/static-check，不能写 PASS。后续在正确 Ubuntu/Linux 环境完成 Server pytest、Agent POSIX/Linux tests 和集成验证后，才升级为 `HOST VERIFIED`。

后续无需为了阶段形式重复已经关闭的 host suites；只有修改相关 contract 时才运行有针对性的 regression。原始早期报告必须保留其当时的 `NOT HOST VERIFIED`，不能回填历史。

## 9. Install outcome uncertainty

R4 已在 host 层验证：install timeout/结果不确定不会对同一 attempt 自动 reinstall。为制造 target 证据而强杀 RAUC 风险过高，R4 不要求该 destructive injection。

如 R6 决定开展 power-cut/RAUC daemon failure campaign，必须使用可恢复实验环境与独立 acceptance plan；在此之前 `same attempt install <= 1` 不变量保持不变。
