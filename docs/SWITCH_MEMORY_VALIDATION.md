# Switch 内存验证：一个 NRO，一次采集

本版把隔离的 Horizon 能力测试、真实 Dynarmic A32→A64 写页表测试、写入微基准，以及游戏中的纹理 staging 缓存维护 A/B 放进同一个模拟器。测试数据留在 RAM，正常结束游戏后写入同一个 TraceLab 目录。无需在 A/B 阶段之间重新编译或修改配置。

这是用于选择下一步内存架构的验证版本。独立写页表已经进入 Dynarmic 后端，但仅在隔离测试里启用；游戏仍使用原来的读写页表。没有把缺少 HLE/GPU 覆盖的脏页实验当成纹理有效性依据。

## 最少操作

1. 用编译产物根目录的 **`Vita3K.nro`** 替换现有启动器 NRO。它内置本次编译的模拟器；`core/Vita3K.nro` 是同一份核心的诊断副本，不需要另装第二个版本。
2. 启动 PCSD00035，进入可重复的重场景。启动过程中会自动执行安全能力测试、JIT 正确性测试和写入微基准。
3. 默认在首次渲染之后预热 **120 秒**，自动开始游戏 A/B。想在选定场景开始或重新开始，只需按一次 **L + R + Y**；按住组合键时不会把该组合继续传给游戏。
4. 在同一场景保持约 **3 分 4 秒**。自动顺序为 **A、B、B、A、B、A、A、B**；每段 3 秒稳定等待 + 20 秒测量。无需中间操作。
5. 正常结束游戏，返回启动器或使用退出菜单。提供这次生成的整个 `sdmc:/switch/vita3k/logs/tracelab-<title>-<time>-<session>/` 目录即可，不用挑选多个 CSV。

如果之前配置了 `mode=off`，诊断仍然遵守关闭设置。把产物中的 `tracelab-memory-validation.cfg` 复制为 `sdmc:/switch/vita3k/tracelab.cfg` 即可恢复上述默认测试流程；这一步只需做一次。该文件独立于启动器生成的 `config.yml`。

预热时打开菜单或失去焦点，会在恢复后重新计时。A/B 已开始后暂停、打开菜单或切换焦点，会使当前一轮无效；恢复到固定场景后再按 L + R + Y。重复按组合键会替换上一次 A/B 结果，但保留启动自测结果。完整的八段跑完后自动恢复 A 策略。

正常退出才导出 RAM 数据；强制杀进程、崩溃或直接断电不能保存尚未导出的这轮采集。阶段变化写入普通日志，不会为每一帧写报告文件。

## 一个版本包含的验证

| 项目 | 自动检查 | 结果用途 |
| --- | --- | --- |
| Horizon 环境 | HOS、loader、SVC hints、Heap/Stack/Alias region、exception context、构建的 libnx 版本 | 区分实际运行环境；hint 不等于能力证明 |
| Heap 权限 | 1、4、16、64 页，各 32 次 RO→RW；核验权限、完整数据和 guard pages | 判断直接 Heap backing 的保护能力及批量 SVC 成本 |
| Stack alias | 独立 Heap→Stack map，源只查询，目标 RO 探测，RW 写入，unmap 后数据恢复 | 在实际模拟器进程里复核映射约束 |
| 原生 A64 store | 独立 CodeMemory `str; ret` 写入 | 演示 native store 不会自动经过软件回调，单列于 Dynarmic 测试 |
| Dynarmic 写页表 | 8/16/32/64 位首次写回调后恢复直接写、读路径不受影响、Run/Step、停止后的重新布防 | 验证独立写表的实际发射和执行语义 |
| 边界和别名 | 同 backing 的 Guest 别名、不连续宿主页、跨页的首/次/双页被监视、无效页回调 | 防止只观察起始页或错误假设宿主页连续 |
| 配置冲突 | write table 缺少 read table、write table 与 fastmem 同时设置 | 在构造时拒绝 X25 所有权冲突，不执行无效配置 |
| 写入微基准 | 原页表 vs 镜像写页表；原页表 vs 每页首次写慢路径；各自 ABBA | 分离换表成本与首次写回调成本，记录实际 Guest store 数 |
| 游戏内 staging A/B | 原缓存维护 vs 非 coherent staging 去重 | 测量保留 Vulkan flush 时省掉重复平台维护的效果 |

`QueryMemory.type` 按查询返回的内存类型记录，不把它与未导出的内核 capability 位进行比较。借用源在 map 期间不解引用。所有权限测试只操作独立分配的内部页，分配边界保留 RW guard，避免改动 allocator 元数据。

如果恢复权限或 unmap 失败，测试记录 `fail` 并保留有关 backing；不会释放仍然受保护或被借用的地址。退出时禁止把此进程交给下一 NRO 做 chainload，改走进程退出。启动的 JIT 测试结束后销毁所有测试页表和 code slice，再关闭空闲测试 code pool，让游戏重新分配自己的资源。

本版不故意触发写保护异常。异常能否安全恢复、Direct Heap Guest backing、生产环境并发重新布防、exclusive store、HLE 和 GPU 写入全覆盖均明确列为未验证。`skip` 表示能力不可用或未覆盖；不等同于 `pass`。

## A/B 到底改变什么

两组始终使用相同的普通纹理缓存参考模式：每次检查重新计算现有 hash 覆盖范围，关闭同 scene 的 hash memo，也关闭历史的两种 unsafe interval skip。旧配置中的 Hash64、guest store counter/guard、hash 分类探针和旧 CSV 采样，在验证模式中都会被强制关闭，避免混入开销或 stale texture 实验。

这个参考模式的范围是 `ordinary_texture_cache_existing_hash_footprint`。它没有替换 surface cache、DoubleBuffer block memo、原有 mip/stride/import hash 范围规则，也没有自动比较 GPU 输出图像。因此 `strict_hash=true` 不是整个渲染器的内存一致性证明。不要把这一版的 A 数字直接和旧版本的 24.36 Loop/s 当成同条件结果；先比较同一 NRO 内的 A/B。

| staging 内存类型 | A：原维护 | B：去重 |
| --- | --- | --- |
| Non-coherent | 平台 cache flush + VMA/Vulkan flush | 保留 VMA/Vulkan flush，省去前一次平台 flush |
| Coherent | 保留现有平台 flush workaround | 相同 |

修改范围只包括 VMA 分配的 texture staging，GPU 作为 TransferSrc 读取它。没有改 External Host Guest pointer 导入、其它 ring buffer、image transition、barrier、fence 或 staging 重用时机。去重的依据是当前固定 Mesa Switch 驱动中 non-coherent flush 的 cache clean 路径；更换 SDK 时必须重新核对，而非推广为所有 Vulkan 实现的结论。

每一轮上传只读取一次策略。计数由渲染线程维护；不增加 Guest 每次 store 的计数，不把 A/B 两组变成不同的原子统计负载。发生很长的跨阶段停顿时，会从实际策略切换时刻重新满足稳定等待，不能仅靠计划时间认定缓存已经稳定。

## 报告与判读

新增 `memory-validation.json` 与既有 `manifest.json`、`summary.json`、`frames.csv`、`hotspots.csv`、`threads.csv`、`nid-samples.csv`、`trace.bin` 位于同一目录。构建产物另附 `build-info.txt` 和 NRO 的 `SHA256SUMS`。

现有分析器会自动读入新文件；只需 Node，不需要 npm 包：

```sh
node analyze-switch-tracelab.mjs "/path/to/tracelab-session" --html memory-report.html > memory-report.json
```

在源码目录使用时脚本路径为 `scripts/analyze-switch-tracelab.mjs`。HTML 包含能力、自测、微基准、八段数据、四个相邻 AB/BA 配对以及排除原因。既有 capture 没有新文件时仍可以分析。

以下条件满足才报告配对性能变化：

- 八段顺序完整、未暂停中断、未 overflow，参考模式一致。
- 每段至少 100 个有效 render-loop 间隔，记录时长至少为目标的 90%。
- JIT 自测没有 `fail`/unknown，测试记录未丢失。
- 对应 B 段确实有 `dedup_calls` 和 `dedup_bytes`。如果所有 staging 都是 coherent，结果为 `n/a`，不能把没有执行的优化报成收益。

记录的速度单位是 **render loops/s**，不是显示 FPS，也不是 LSFG 输出帧率。还给出 p50/p95/p99，以及因 settle/boundary 排除的间隔和时长。微基准的 `ns/store` 包含循环和 JIT 调度等配套成本，没有减去计时/循环 overhead，不应解读成裸 STR 指令 latency。

能力失败和微基准失败始终列在报告顶部；不支持的能力 `skip` 不会让整轮错误地显示“全部通过”。实际画面是否出现花屏、旧纹理、渲染目标反馈异常，仍需在同一重场景观察。请连同这些现象和当时的时钟、LSFG/渲染设置一起反馈；A/B 中途不要改变这些条件。

## 可选配置

```ini
mode=auto
memory_validation=auto
validation_warmup_seconds=120
validation_settle_seconds=3
validation_phase_seconds=20
```

`memory_validation=on` 仅取消自动启动，仍自动执行启动自测，游戏 A/B 由 L + R + Y 开始。`memory_validation=off` 关闭本次验证功能；`mode=off` 同时关闭 TraceLab 和验证。时长范围分别为预热 0–600 秒、稳定等待 1–30 秒、测量 5–120 秒。通常直接使用默认值。

## 开发验证与后续 gate

CI 仍只编译一次核心和一次内嵌核心的启动器，没有 A/B build matrix。编译前运行 host preflight，覆盖控制器的真实状态机、暂停退出、重新开始、跨段停顿、有界记录、延迟导出、分析器排除规则及 staging 发布决策；交叉编译检查实际 Switch 类型和链接。

host 测试不能替代 Horizon SVC 或 Cortex-A57 的实际执行结果。收到这一份实机报告后，才能决定是否把写表接入真实 Guest 内存，并进一步增加 backing/alias 身份、CPU safepoint 重新布防、跨页回调、HLE 受控写入和 GPU owner/version 协议。完整覆盖前不启用“页未脏就免 hash”的游戏策略。
