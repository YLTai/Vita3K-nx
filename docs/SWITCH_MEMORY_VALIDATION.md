# Switch 内存验证：一个 NRO，一次采集（v4）

当前实验是 **真实首次写入观察 + 指定 HLE 完成通知 + 缓存变化逐条对账**。
完整内容 Hash 始终保留；没有启用基于脏页的 Hash 跳过。

完整说明见 [SWITCH_WRITE_AUDIT_V4.md](SWITCH_WRITE_AUDIT_V4.md)。构建产物中对应文件为
`READ-WRITE-AUDIT-V4.md`，本页为 `READ-MEMORY-VALIDATION.md`。

## 最少操作

1. 替换产物根目录的 **`Vita3K.nro`**；它内嵌同一核心，`core/` 仅为诊断副本。
2. 启动 PCSD00035。能力/JIT 回归、100 样本写表矩阵和新增的 24 条正反例结果自动运行。
3. 进入固定重场景，按一次 **L + R + Y**，自动执行 A/B/B/A。
4. 默认每段 3 秒稳定等待、20 秒测量，共约 **92 秒**。看右下角 overlay，不需要自己计时。
5. 任意 **DONE** 出现后正常退出，将一个完整 TraceLab 目录交回。不因黄色或红色结果盲目重跑。

启动自测在 renderer 创建前执行，首帧后的面板显示自测汇总；不声称有启动自测进度画面。
面板独立于 FPS 开关，最多 4 Hz 更新，不额外发起 present。
完成提示为 `EXIT GAME NORMALLY TO SAVE`：此时数据还在 RAM，正常退出才写入 SD。
不要强制关闭、断电或用 HOME 杀进程；这些操作可能丢失采集。

## 状态与退出

`WARM-UP` 是自动开始前的预热；组合键可立即开始。`SETTLING` / `MEASURING`
显示段号、A/B、进度和剩余时间。运行中菜单/失焦使当前轮中断；恢复固定场景后组合键重启。
已经完成后打开退出菜单不会使结果失效。

`DONE - CAPTURE READY` 只是采集质量通过，不是画面正确性证明。
`DONE - NO LIVE CHANGES` 表示自然变化不足以检验覆盖，仍应交回报告。
`DONE - INCONCLUSIVE` / `CHECK REPORT` 也保存交回，不要为获取绿色状态反复操作。
计时器归零不能独自触发 DONE，状态由实际源渲染循环推进。

## 配置兼容

旧配置没有 `validation_experiment` 时，本 TraceLab 构建默认使用 `write_shadow`。
`mode=off` 仍关闭 TraceLab 与验证；此前关掉诊断的用户只需一次复制附带的配置预设。
`memory_validation=auto` 默认首次渲染后预热 120 秒自动开始；`on` 则只由组合键开始。

```ini
mode=auto
memory_validation=auto
validation_experiment=write_shadow
validation_warmup_seconds=120
validation_settle_seconds=3
validation_phase_seconds=20
```

`validation_experiment=staging` 保留旧八段开发对照，但不是本轮要求。
所有验证仍覆盖旧 unsafe Hash64 / store-probe 设置，以免混入不可靠跳过策略。

## 分析与边界

将完整会话目录交回即可；开发者也可执行：

```sh
python3 analyze-switch-memory-v3.py /path/to/session --html write-report.html > write-report.json
node analyze-switch-tracelab.mjs /path/to/session --html tracelab.html > tracelab.json
```

第一个脚本保留旧文件名但支持 v4；第二个用于原始 TraceLab 热点和帧数据。
`memory-validation.json` 顶层 schema 保持 1，`validation_revision=4`，write_shadow schema 为 2。
只将结构、回调合同与配对检查均完整的数据用于开销比较。

A/B 都有镜像写表、跨页检查和观察器公共开销，不能直接与旧无插桩基线比较。
本轮只添加六条 libc/clib 的写完成通知，不等于所有 HLE/GPU 写入已覆盖。
没有 all-JIT safepoint、可恢复写保护异常、Direct Heap 或新增 GPU 图案读回测试。
零未归因变化不证明可以取消 Hash；CPU/HLE-seen 也不是每次变化的因果证明。
