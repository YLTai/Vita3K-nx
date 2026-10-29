# Switch 内存验证 v3：一次采集，进入真实游戏写入观察

## 本轮为什么改方向

上一轮 `1793251495` 的 v2 日志包含 167 条结果：161 PASS、6 SKIP、0 FAIL。
100 个启动微基准样本有 99 个通过质量筛选，14/16 个比较有完整配对。
共享同一 JIT 后，镜像写表与首次写入处理的差别很小，未复现之前约 42% 的异常；
这不证明生产追踪零成本，但已经足够进入真实游戏验证，不再单独要求一轮微基准。

八段 staging 比较虽然结束，最后一段 B 没有任何上传；前三个有效相邻配对方向也不一致。
不应让用户为该静态场景反复重跑 staging。130–320 秒窗口依然约 10.09 Loop/s，
每轮约 3,301 次 Hash、253.87 MiB 逻辑扫描，采样外推约 68.78 ms/轮；
缓存清理另约 3.74 ms/轮。嵌套计时不能相加，逻辑扫描量也不等于 DRAM 流量。

## 最少操作：只换一个 NRO、只交一个目录

1. 用构建产物根目录的 **Vita3K.nro** 替换启动器。它已经内嵌本次核心；不必另装 `core/`。
2. 启动 PCSD00035。既有能力、自测与 100 样本矩阵仍自动运行，作为回归，不需要额外操作。
3. 进入可重复的重场景后按 **L + R + Y**。默认仍有首次渲染后的 120 秒自动预热；组合键直接开始/重启采集。
4. 本轮自动执行 **A → B → B → A**，默认每段 **3 秒稳定等待 + 20 秒测量**，合计约 **92 秒**。
5. 面板出现 **DONE** 后，正常退出保存，把一个完整 TraceLab 会话目录交回即可。出现 `Unobserved`、`INCONCLUSIVE` 或 `CHECK REPORT` 也先交报告，不要盲目重复。

配置文件仍是 `sdmc:/switch/vita3k/tracelab.cfg`。老配置没有 `validation_experiment` 时，
本次 Switch TraceLab 构建默认选择 `write_shadow`，不必改文件。`mode=off` / `memory_validation=off`
仍被遵守；需要开启时使用产物内预设一次即可。

## Overlay 怎么看

独立于 FPS 开关的原生面板继续显示阶段、A/B、进度、当前段/总剩余时间和自测结果。
新增 `Pages`、`First writes`、`Unobserved`，并始终显示 `FULL Hash`。

| 状态 | 操作 |
| --- | --- |
| WARM-UP / SETTLING / MEASURING | 保持场景；采集中不退出 |
| INTERRUPTED | 菜单/失焦/暂停使未完成的一轮无效；恢复场景后可按组合键重启，或直接交部分报告 |
| DONE - CAPTURE READY | 基本采集质量通过；正常退出保存，不代表画面正确性已获证明 |
| DONE - INCONCLUSIVE | 缺少监视资源覆盖等；正常退出交报告，不要求重跑 |
| DONE - CHECK REPORT | 自测、访问或记录质量异常；正常退出保留证据 |

未观察到 CPU 写入但 Hash 变化时，完成面板给出黄色提示；它是有用的调查结果。
明确的失败仍保持红色，不被黄色提示覆盖。完成后打开退出菜单不会破坏结果。
倒计时不能自行宣布 DONE；状态来自实际渲染循环。显示与 v2 相同，最多 4 Hz 更新，
不新增 present 或游戏期间的报告写盘。启动隔离测试仍在 renderer 创建前执行，
没有启动阶段 overlay；第一帧后能看到启动测试汇总。

## A/B 实际改变什么

两组都使用独立的 X25 写页表、X24 读页表，以及跨页慢路径检查。
普通非监视写入没有新增逐 store 计数器或 bitmap 查询。纹理仍执行原有完整内容 Hash，
Hash 结果仍是上传决策依据。所有旧 unsafe interval skip 和逐写探针被验证模式关闭。
本轮不启用 staging 去重，也不改变 Vulkan barrier、fence 或资源所有权策略。

- **A**：写页表开放，收集实际被 Hash 的 pixel/palette 宿主范围，建立后备页到资源的依赖。
- **B**：段开始时扫描一次正常页表，找出已登记后备页的 Guest 软件别名，把相关写表项设为零。
  首次成功写入在既有回调处记录，随后一起开放该后备页的全部已知软件别名。
  后续普通写入重新直达，不为每次写做计数。
- 第二个 B 重新建立观察 epoch。普通读取、成功/失败 CAS、跨页访问和映射替换使用明确的观察路径。
- B 中首次出现而此前未登记的后备页，标作 **uncovered**，直到后续 B 才可能纳入；不会当成可靠未写入证据。

这个 A 是带相同表和边界检查的内部基线，不是旧版原始 NRO；A/B 能测首次写入观察的增量，
不能独立量化全部新表、跨页检查及共同报告开销。不要与旧版 24.36 Loop/s 直接比较。

## 重要边界：这是观察器，不是 hashless 脏页系统

报告显式写入 `hash_skip_authority=false`、`arm_safepoint=false`、`HLE_GPU_coverage=false`。
段切换尚未停止全部 Guest 写入者，不能撤销其他核心已经载入的旧 PTE；
“未见 CPU 写入但 Hash 变了”可能来自 HLE/GPU/原始宿主指针，也可能来自并发或登记窗口。
不能仅凭该事件判断具体写入者。`changed_cpu_seen` 只表示该 B epoch 曾见相关页写入，
并不证明每次内容变化都由这次 CPU 写入造成。

范围只覆盖 `hash_texture_data` 的既有字节范围，包括其 palette；
不覆盖 surface-cache 命中、纹理替换/导出 `hash_texture_nostride` 路径、GPU 私有图像或所有 HLE 写入。
同一宿主虚拟后备页的 Guest 别名能联动；不同宿主映射指向同一物理页并未自动发现。
映射锁保护元数据与回调，不会让原生快路径取得同一把锁，也不声称解决原来所有并发 free 问题。

出现未知变化仍使用完整 Hash 更新画面；**零未知事件也不是安全跳 Hash 的证明**。
可恢复硬件写保护异常、Direct Heap Guest backing 和新的 Vulkan 上传—图像—读回图案测试不在本次提交中。

## 有界数据与退出报告

新增表约 8 MiB，另有上限明确的元数据：1,024 个精确 footprint、16,384 个后备页、
131,072 条依赖边、128 条详细事件。容量不足会标记部分覆盖；不会用未登记资源作正确性证据。
详细首次写/映射事件分别限额，为未知 Hash 变化留出空间；丢事件有计数，累计分类仍保留。
分配失败开放写表并停止观察，不得留下阻塞项；正常 teardown 在释放 Guest 表前 detach。

`memory-validation.json` 新增 `write_shadow`：四段计数、实际 armed 页和别名数、
首次 CPU 写入数、完整 Hash 检查数、covered/uncovered、变化分类、映射更新、
拒绝范围/非法访问，以及带 tick、Guest 地址、大小、Hash 前后值的详细事件和 footprint 索引。
计数包含各段稳定等待，吞吐率只用测量区间，不能把两者误当同一精确窗口。
报告仍与既有 manifest / frames / hotspots / threads / trace 在同一目录，正常退出才写盘。
强制结束、崩溃或断电可能丢失 RAM 数据。

## 一条分析命令

```sh
python3 analyze-switch-memory-v3.py /path/to/tracelab-session --html write-watch.html > write-watch.json
```

分析器检查完整 ABBA、采样时长与数量、自测、访问错误、覆盖与计数一致性。
不完整比较不输出速度比。容量/详细事件截断会单独警告。原 `analyze-switch-memory-v2.py`
仍分析启动矩阵；Node 分析器保留通用热点/帧数据，但其旧八段 staging 比较不适用于 v3。

## 可选配置与可重复构建

```ini
mode=auto
memory_validation=auto
validation_experiment=write_shadow
validation_warmup_seconds=120
validation_settle_seconds=3
validation_phase_seconds=20
```

`validation_experiment=staging` 保留旧八段流程，仅用于开发回归，不是本轮推荐运行。
`memory_validation=on` 取消自动开始；启动测试保留。时长范围与 v2 相同。

实验接线以 `patches/experiments/switch-live-write-watch.json` 隔离在 mem、CPU 回调和纹理 Hash 三处。
**这是构建前自动应用的源代码接线配方**，不是运行时下载或补丁 JIT 机器码。
配方锁定整个原始 Git blob，逐项要求唯一锚点；源码变化时拒绝模糊应用。
CI 在已有子模块补丁之后自动运行，所有文件验证通过后才写入；重复执行无副作用。
CMake 的 Switch TraceLab 配置拒绝遗漏接线的构建。手工构建时运行：

```sh
bash scripts/apply-switch-submodule-patches.sh
python3 scripts/apply-switch-write-watch-hooks.py
# 然后使用既有 Switch CMake/build 命令
```

产物附 `write-watch-hooks.json`，记录配方 SHA256 与三份实际编译源码的 Git blob 身份；
编译 SHA 必须结合这些身份理解。后续正式化时应把接线整理为普通源代码改动。
仍只编译一次核心与一次内嵌启动器，不做 A/B build matrix。

本地主机回归覆盖真实观察器、别名、跨页、失败/成功 CAS、映射重用、边界、
有界记录、并发元数据访问、真实控制器/运行时 API/HUD/导出与分析器。
这不替代 native JIT、Horizon、Vulkan 或多核心 PTE 撤销验证。完整项目 preflight 和
Switch 交叉编译由 CI 执行，实机结果由这一次采集取得；不要在这些 gate 前合并草稿。
