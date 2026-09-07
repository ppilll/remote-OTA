# EdgeGuard Remote OTA R3 交接包

冻结日期：2026-09-07  
适用平台：ATK-DLRK3588 / RK3588  
当前硬件闭环：`1.2.3 / rk3588-r3h-1.2.3-001` → `1.2.4 / rk3588-r3h-1.2.4-001`

本包只保留推进后续正式升级所需内容：

- `01_FORMAL_UPGRADE_SOP.md`：Ubuntu 构建/发布与 RK3588 Agent 驱动升级。
- `02_R3_TECHNICAL_HANDOFF.md`：R3 架构、冻结证据、安全不变量、已验证与未验证项。
- `03_COMMAND_CHEATSHEET.txt`：极简命令速查。

使用顺序：先读 SOP，再把技术说明作为后续 Chat/Codex 的上下文。速查文件只适合已理解前提时直接使用。

范围声明：本包不重新设计 Native A/B，不重复已通过的 host/SDK tests，不包含 Git hygiene，也不把后续故障注入场景误写成已验证。

