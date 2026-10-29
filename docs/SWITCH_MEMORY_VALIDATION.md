# 当前实验：私有 Heap 写保护异常恢复，一次启动完成

## 为什么结束上一轮

实机 capture `1793271342`（源提交 `1490a2fc`）中，完整字节快照比较
C 为 7.412535 Loop/s，完整 Hash A 为 10.133981 Loop/s，吞吐下降 26.855%。
B 的 415,781 次逐次审计无差异，但不能抵消性能退步。默认不再分配纹理快照，
不再重复 72 秒静态扫描比较。旧实验仅保留为显式 `legacy_content_compare` 开发回归。

## 本次最少操作

替换成功构建产物根目录的 `Vita3K.nro`，启动游戏即可。测试自动在真实 Guest
启动前执行；进入游戏首帧后，原生面板显示结果。无需进入固定重场景、按组合键
或再等 72 秒。看到 `DONE - REPORT READY` 或 `DONE - CHECK REPORT` 后，正常
退出保存，提交一个完整 TraceLab 会话 ZIP。

GPU 导入能力测试在第一次 External Host 映射时自动完成；如果游戏从未走该路径，
报告明确为 not_exercised，不假装通过。面板会显示是否检测到该测试及 RO 能力。
启动的原生/JIT 测试发生在 renderer 创建之前，不能在尚不存在的 overlay 上绘制
逐测试进度；首帧显示汇总。本版没有额外的游戏计时阶段。

**这次会故意向独立 RO Heap 页写入，以验证能否恢复。首次尝试存在应用退出风险。**
每个危险阶段前写并 fsync `sdmc:/switch/vita3k/logs/heap-fault-<revision>.json`。
如果应用异常退出，提供该文件和 crash log；不要反复重试。相同构建下一次启动
检测到未返回/不完整标记会禁止再触发故障，并把标记复制进正常导出的会话目录。
正常返回的阶段标记不代表全部控制通过，仍以报告为准。

已有配置未提供新键或写着旧 content_compare/compact/write_shadow 时，默认进入
本轮；显式 `mode=off` 或 `memory_validation=off` 仍关闭实验。

## 一版集中验证的内容

- 原生 8/16/32/64/128 位 store：原 PC 恢复、首次 fault 后第二次直接写。
- 寄存器实际前后比较：全部 X/Q、SP、NZCV、FPCR/FPSR；故意异常期间不改 frame PC。
- 128 位跨页 store、四页裸宿主 `memset`、64 轮重新保护/写入/恢复及计时。
- 实际 Dynarmic 普通页表直接 store：8/16/32/64 位、两个 Guest 地址的软件别名。
  成功条件包括硬件 fault 发生、MemoryWrite 软件回调为零和数据/guard 正确。
- 私有 Vulkan External Host buffer 的导入前、绑定后、解除导入后 RO/RW 能力。
  这里只导入，不提交 GPU 工作，不宣称捕获 GPU 写入或证明 IPC 输出兼容。
- 原有自动隔离控制继续作为回归；不重复性能矩阵或 staging A/B。

## 实现边界

链接器只包装 libnx 的异常入口。默认入口的 PLR 小栈不足以保存全部 SIMD 状态，
所以先切换到独立 16 KiB 异常栈，再保存现场。处理器只接收同测试线程、登记的
私有 Heap 范围内、有效 FAR 的 A64 写权限异常；原生测试还匹配指定 PC。
处理器内无分配、日志、mutex、文件操作、GPU 调用或等待，只解除对应页保护。
恢复寄存器后执行 ReturnFromException，内核重试原指令；其他异常仍走 libnx
原 crash handler。异常栈不位于被保护范围内。

未确认恢复 RW 的 scratch backing 不返还给 allocator；禁止带残留保护 chainload
下一个 NRO。内核用户异常串行规则不等于已经证明多线程追踪正确，本版只做
单写入线程控制。软件 Guest 别名指向同一宿主 VA；不同宿主 VA 的物理别名未测。

**游戏仍使用原映射、原 JIT 和普通完整 Hash/场景 memo 策略。** 不保护游戏内存，
不启用固定间隔跳过，不启用当前失败的快照候选。没有完成 Direct Heap Guest
转换、所有 JIT 安全点、完整 HLE/GPU 所有权或通用 hashless 缓存。

## 结果与停止条件

`memory-validation.json` 的 `capture_plan` 为 `heap-fault-1`，同时复制
`heap-fault-checkpoint.json`。构建内仍附一个分析器，支持目录或 ZIP：

```sh
python3 analyze-switch-capture.py tracelab-session.zip --html report.html
```

CPU 恢复控制通过与 GPU 导入后可否改权限分别报告。GPU 导入后返回拒绝但保持 RW
是能力边界（skip），不是让你重跑的理由。任何失败、缺项或未覆盖直接共享报告。
64 轮平均值包括布防、异常、重试与恢复，不是单独裸 fault latency，更不是游戏收益。

后续预算按 **2–3 轮决策与验收**：本轮解决异常恢复与 GPU 导入约束；通过后把可追踪
Heap 路径和不兼容范围回退放入同版游戏对照；最后用动态场景回归。这个预算不是
保证所有资源在三轮内无 Hash。能力失败时本轮就决定替代路线，不再重复静态扫描。

## 构建/主机验证

原有完整 preflight 之外，新增实际 handler 的 POSIX mprotect/合成异常帧测试、
真实控制器/导出/HUD、A64 汇编与 --wrap 链接检查。主机测试不冒充 Horizon
异常投递、原 PC 执行、实际 GPU 导入或实机性能。最终 ELF 另校验入口确实跳至 wrapper。
首轮实机结果之前保持 PR 草稿。
