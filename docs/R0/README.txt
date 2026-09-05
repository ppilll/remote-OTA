EdgeGuard Remote OTA - R0 Deliverables

状态：R0 PASS（2026-08-31）

文件：
00_R0_阶段结项报告.docx
01_R0_产品定义与架构基线.docx
02_R0_硬件_BSP_无线能力基线.docx
03_R0_存储_AB与Boot控制基线.docx
04_R1_移交与验收入口.docx

关键结论：
- RK3588/Buildroot/Linux/RTL8733BU BSP baseline 已验证。
- Wi-Fi AP 扫描与 BLE HCI/BlueZ 已验证。
- Vendor SDK A/B partition capacity 可行。
- 当前 U-Boot 没有标准持久化 env backend，CONFIG_ANDROID_AB 未开启。
- 后续优先评估 Rockchip misc A/B + RAUC custom backend；必要时回退到新增持久化 U-Boot env + RAUC standard U-Boot backend。
- RTC 电池待更换，必须在 HTTPS/TLS 验证前关闭。
