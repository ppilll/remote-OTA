# EdgeGuard Remote OTA R4 Corrective Review

## 修改文件

- `host_app/models.py`
- `buildroot-external/package/edgeguard-remote-ota/edgeguard-health-check`
- `RK3588_app/edgeguard-remote-ota-agent/tests/r4_thread1/test_health_hook.py`
- `tests/test_reports_and_logging.py`
- `CODEX_REPORT/R4_CORRECTIVE_REVIEW.md`

## 三个问题如何修复

1. `host_app/models.py`
   - 将所有 `X | None` 改为 Python 3.8 可解析的 `Optional[X]`。
   - `Annotated` 改从 `typing_extensions` 导入，避免 Python 3.8 的 `typing` 不提供该名称。
   - 保留原有 extended reporting 字段、`StrictStr`/`StrictBool`/`StrictInt` 严格类型、显式 `null` 拒绝、枚举范围、UTF-8 字节边界、ASCII 控制字符拒绝和 `uptime_ms` 有符号 64 位范围。
   - 未修改 Agent C reporting wire format。

2. `edgeguard-health-check` 与对应 fixture
   - 保持 `REQUIRED_STREAK=3`、`SAMPLE_INTERVAL_SEC=5`，将 `MAX_SAMPLES` 从 `6` 改为 `5`。
   - fixture 的失败序列、调用次数和 sleep 次数同步为最多 5 次采样、4 次间隔；仍覆盖失败后 streak 清零、最后连续 3 次成功才通过，以及非连续成功不提前通过。
   - health 语义未变：只观察 `wlan0`、`connmand`、`wpa_supplicant` 和 ConnMan Wi-Fi technology；没有加入 AP、DHCP、DNS、Internet 或 Server reachability 检查，也没有加入 enable/connect 或其他状态变更操作。

3. `tests/test_reports_and_logging.py`
   - 删除第二份完全重复的 `AGENT_ERROR_CODES`。
   - 删除与保留用例参数和断言完全相同的一组 Agent state/error-code 接受测试。
   - 保留一组覆盖全部 15 个 Agent state 和全部 29 个 Agent error code 的参数化测试；其他 legacy、extended、严格类型、边界、显式 `null`、未知字段和日志测试均保留，实际覆盖场景未减少。

## 实际执行的测试及结果

- `python RK3588_app/edgeguard-remote-ota-agent/tests/r4_thread1/test_health_hook.py`：6/6 通过。
- `python RK3588_app/edgeguard-remote-ota-agent/tests/test_core_source_contract.py`：7/7 通过。
- `python RK3588_app/edgeguard-remote-ota-agent/tests/test_merged_source_contract.py`：6/6 通过。
- `python RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_package.py`：7 个通过，1 个跳过。
- `D:\Git\bin\sh.exe -n buildroot-external/package/edgeguard-remote-ota/edgeguard-health-check`：通过。
- `D:\Git\bin\sh.exe -n buildroot-external/package/edgeguard-remote-ota/post-build-check.sh`：通过。
- `python -m compileall -q host_app tests RK3588_app/edgeguard-remote-ota-agent/tests`：Python 3.8.5 下通过。
- Python 3.8.5 对 `host_app/models.py` 的直接编译及 `Optional`/`typing_extensions.Annotated` 导入检查：通过。
- `python -m pytest -q`：已启动，但在收集 `tests/conftest.py` 时因当前环境缺少 `fastapi` 返回退出码 1；没有 Server 用例被执行，因此不计为通过。

## Skipped 项

- 完整 Server pytest：当前两个可用 Python 3.8 环境均缺少 `fastapi`、`pydantic`、`starlette` 和 `httpx`；临时依赖安装未获准/未成功，测试在收集阶段停止。Legacy、extended reporting、manifest、artifact 和 Range 回归均不声称 PASS。
- `test_package.py::test_final_target_check`：Windows 文件系统不能验证 POSIX target executable mode，按测试自身门禁跳过。
- `tests/thread3/test_preinstall.py`：当前主机不是受支持的 POSIX shell 环境，报告 `SKIPPED / NOT HOST VERIFIED`。
- `tests/thread3/run_tests.py`：缺少 POSIX C compiler 和/或 `pkg-config`，报告 `SKIPPED / NOT HOST VERIFIED`。
- `tests/run_merged_tests.py`：缺少 POSIX C compiler 和 `pkg-config`，报告 `SKIPPED / NOT HOST VERIFIED`。
- 未执行真实 Agent、ConnMan、Wi-Fi、RAUC、reboot、A/B、mark-good、mark-bad、rollback 或 unattended OTA 运行时测试。

## Remaining VM/SDK/target validation

- VM/Ubuntu：安装锁定的 Server 依赖并运行完整 `pytest`，保留 legacy/extended report、结构化日志、manifest、artifact/full-download 和 Range 回归结果。
- VM/Ubuntu：运行需要 POSIX C compiler、`pkg-config` 和开发包的 core/merged Agent C 测试及 pre-install 测试。
- Vendor SDK/Buildroot：交叉编译并检查最终镜像中的 hook 路径、`0755` 模式、30 秒配置和 post-build comparison；确认既有 RAUC 1.5.1/custom backend/Native A/B 架构保持不变。
- RK3588 target：验证真实 `wlan0`、`connmand`、`wpa_supplicant`、ConnMan technology 行为和 5 次采样在 30 秒 hook timeout 内的实际裕量。
- RK3588 target：执行文档规定的 health acceptance/rollback、report retry、AP/server reachability independence 和 unattended OTA campaigns，并保留相应证据。
