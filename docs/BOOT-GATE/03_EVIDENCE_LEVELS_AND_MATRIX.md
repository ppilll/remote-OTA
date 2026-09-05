# 证据等级与冻结矩阵

## 六种等级

这些标签描述“证据来自哪里”，不是模糊的完成百分比。

| 等级 | 允许的依据 | 不能替代什么 |
|---|---|---|
| `SOURCE VERIFIED` | 已阅读当前项目/当前 vendor SDK 的实际源码，调用链与语义明确 | 不能证明配置已启用、构建成功或目标板已运行 |
| `SDK CONFIG VERIFIED` | 当前 active SDK `output/.config`、最终 U-Boot `.config` 等生成配置已核对 | 不能证明二进制已生成或镜像已打包 |
| `SDK BUILD VERIFIED` | 对应构建命令成功，日志、二进制和来源匹配 | 不能证明最终 package 内容或实板行为 |
| `OFFLINE ARTIFACT VERIFIED` | staging、parameter、镜像链接/文件、最终 update image 的离线内容或映射已检查 | 不能证明刷写成功或 bootloader 在板上按预期工作 |
| `TARGET VERIFIED` | 真实目标板 Linux/U-Boot 状态、分区、挂载、重启和回退行为已观察 | 不自动证明物理灾难恢复流程 |
| `HARDWARE VERIFIED` | 必须进行物理交互或故障注入的能力已实际演练，如 Loader/Maskrom 进入、恢复刷写、断电场景 | 不能由文档、源码、正常启动或“工具应该支持”替代 |

同一功能可在多个层级分别获得证据。例如 system slot 选择既有 `SOURCE VERIFIED`，也有最终 `TARGET VERIFIED`。后者不会抹掉前者，两者说明不同的链路部分。

## 冻结矩阵

| 项目 | 证据等级 | 冻结结论 |
|---|---|---|
| 本地 U-Boot `system -> system_a/system_b -> PARTUUID` 调用链 | `SOURCE VERIFIED` | 已闭合 |
| 本地 A/B update packing 脚本对 A/B 镜像的生成逻辑 | `SOURCE VERIFIED` | 已确认首次完整 update 会覆盖 A/B 两组 |
| active SDK 选择 `RK_AB_UPDATE=y` | `SDK CONFIG VERIFIED` | 已确认 |
| `RK_PARAMETER=parameter-ab.txt` | `SDK CONFIG VERIFIED` | 已确认 |
| `RK_UBOOT_CFG=alientek_rk3588` | `SDK CONFIG VERIFIED` | 已确认 |
| `RK_UBOOT_CFG_FRAGMENTS=alientek_rk3588-ab` | `SDK CONFIG VERIFIED` | 已确认 |
| `CONFIG_ANDROID_AB=y` | `SDK CONFIG VERIFIED` | 已确认 |
| `CONFIG_SPL_AB=y` | `SDK CONFIG VERIFIED` | 已确认 |
| `CONFIG_AVB_LIBAVB_AB=y` | `SDK CONFIG VERIFIED` | 已确认 |
| `CONFIG_SPL_KERNEL_BOOT` disabled | `SDK CONFIG VERIFIED` | 已确认 |
| A/B U-Boot 以 Alientek base + A/B fragment 构建 | `SDK BUILD VERIFIED` | PASS |
| matching U-Boot/SPL/loader artifacts 生成 | `SDK BUILD VERIFIED` | PASS |
| Buildroot rootfs 构建 | `SDK BUILD VERIFIED` | PASS |
| firmware 和 update image 构建 | `SDK BUILD VERIFIED` | PASS |
| firmware `parameter.txt` 实际指向 `parameter-ab.txt` | `OFFLINE ARTIFACT VERIFIED` | PASS |
| firmware staging 含 matching loader、U-Boot、boot、rootfs、update image | `OFFLINE ARTIFACT VERIFIED` | PASS |
| `output/firmware/update.img -> update-ab.img` 且真实 update image 生成 | `OFFLINE ARTIFACT VERIFIED` | PASS |
| 最终 package-file 的完整原文与哈希清单 | 未保存在现有证据中 | 后续若要完全复现应重新采集；不得杜撰 |
| 完整 `update.img` 刷入真实开发板 | `TARGET VERIFIED` | PASS |
| 目标 GPT 含 `boot_a/b`、`system_a/b` | `TARGET VERIFIED` | PASS |
| A suffix、A PARTUUID、`/` 实际来自 system_a | `TARGET VERIFIED` | PASS |
| B suffix、B PARTUUID、`/` 实际来自 system_b | `TARGET VERIFIED` | PASS |
| Rockchip metadata 格式/offset/size/version/CRC | `TARGET VERIFIED` | PASS |
| A/B set-primary 与 mark-good | `TARGET VERIFIED` | PASS |
| successful 槽普通重启不消耗 retries | `TARGET VERIFIED` | PASS |
| pending 槽 retries 递减 | `TARGET VERIFIED` | PASS |
| retries 耗尽、canonical bad、自动 fallback | `TARGET VERIFIED` | PASS |
| bad 槽可重新激活、启动和确认 | `TARGET VERIFIED` | PASS |
| 最终 A primary、A/B confirmed-good | `TARGET VERIFIED` | PASS |
| RAUC custom backend | 未验证 | `NOT STARTED / NOT VERIFIED` |
| RAUC bundle install、切换、health、mark-good、rollback | 未验证 | `NOT STARTED / NOT VERIFIED` |
| Loader/Maskrom 实际进入 | 无独立记录 | `NOT HARDWARE VERIFIED` |
| Loader/Maskrom 恢复刷写 | 无独立记录 | `NOT HARDWARE VERIFIED` |

## 证据使用规则

- 不把 `SOURCE VERIFIED` 写成 “目标板已经验证”。
- 不把 `SDK CONFIG VERIFIED` 写成 “二进制肯定包含此功能”。
- 不把 `SDK BUILD VERIFIED` 写成 “update.img 内一定映射正确”。
- 不把 `OFFLINE ARTIFACT VERIFIED` 写成 “已经实际启动”。
- 不把 Linux 正常重启写成 Loader/Maskrom `HARDWARE VERIFIED`。
- 没有保存的 hash、commit、package-file 原文应标记缺失，不能根据成功现象反推具体值。

