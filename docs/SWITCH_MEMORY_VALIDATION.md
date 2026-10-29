# Switch 内存验证 v2：一个 NRO，看进度完成一次采集

本版在 PR #13 原有验证上增加独立的原生 overlay，以及一轮启动即可执行的 100 样本写表实验矩阵。仍只编译一份核心和一份内嵌该核心的启动器，不要求更换多个 A/B NRO 或在阶段之间修改配置。

**独立写表目前只用于隔离自测，游戏仍使用原来的 JIT 页表。没有启用未经完整 JIT/HLE/GPU 一致性验证的免 Hash 策略。** overlay 中的完成表示“采集结束”，不表示“画面正确性已经证明”。

## 最少操作

1. 用产物根目录的 **`Vita3K.nro`** 替换启动器。它已内置新核心，`core/Vita3K.nro` 只是诊断副本，不必另装。
2. 启动 PCSD00035。Heap/Stack、JIT 正确性和新写表矩阵全部自动运行。初始自测在 renderer 创建前执行，此时尚无游戏 overlay；第一帧后面板会显示启动自测结果汇总。
3. 进入固定重场景，按一次 **L + R + Y** 开始；或沿用首次渲染后 120 秒自动启动。组合键不会继续传给游戏。
4. 看右下方 **MEMORY VALIDATION** 面板。八段自动按 **A、B、B、A、B、A、A、B** 执行，每段 3 秒稳定等待和 20 秒测量。默认整轮约 184 秒，不用手动切换。
5. 面板出现 **DONE** 后，按照 **EXIT GAME NORMALLY TO SAVE** 提示正常退出游戏。把这次完整的 `sdmc:/switch/vita3k/logs/tracelab-<title>-<time>-<session>/` 文件夹提供出来即可，不需要逐个处理 CSV。

此前设了 `mode=off` 的用户，只需把产物中的 `tracelab-memory-validation.cfg` 复制为 `sdmc:/switch/vita3k/tracelab.cfg` 一次；诊断仍尊重关闭设置，不会擅自忽略 `mode=off`。预设独立于启动器生成的 `config.yml`。

## 面板状态与退出时机

面板独立于 FPS overlay 开关，采用现有 Vulkan 原生 overlay。文字和进度条最多每秒更新四次；没有输入线程、额外的 present 或游戏期间报告文件写入。固定大小和刷新策略对 A/B 相同；自身仍有绘制开销，因此不要直接拿本版速度与无面板的旧版本比较。

| 状态 | 含义 | 操作 |
| --- | --- | --- |
| WARM-UP | 自动启动倒计时 | 进入稳定场景；L+R+Y 可立即开始 |
| READY TO START | 仅手动模式 | 在选定场景按 L+R+Y |
| SETTLING | 当前 A/B 策略刚切换，等待稳定 | 保持场景，不退出 |
| MEASURING | 正在采集，显示段号、A/B、总体百分比、段内和总剩余时间 | 保持场景，不退出 |
| START / RESTART REQUESTED | 已收到按键，等待下一个 source render loop 处理 | 不要立即退出 |
| PAUSED before capture | 尚未开始时进入菜单/失焦 | 恢复后重计预热 |
| INTERRUPTED | 采集中暂停、打开菜单或失焦；当前轮失效 | 恢复场景后 L+R+Y 重启，或正常退出提供部分报告 |
| DONE - CAPTURE READY | 八段完成，基本样本量、记录完整性和 B 实际执行检查通过 | **正常退出保存，不必继续挂机** |
| DONE - INCONCLUSIVE | 八段完成，但至少一段没有实际 B 上传 | **正常退出并提供报告**；不要盲目重复，不能据此声称优化有效 |
| DONE - CHECK REPORT | 自测、样本质量或记录完整性检查失败 | **正常退出并提供失败记录**；不要当作无损优化通过 |
| WAITING FOR SOURCE FRAMES | source render loop 长时间没有更新 | 进度保持最后一次完成位置，不会凭计时器虚构 DONE |

面板的 PASS / FAIL / SKIP 是结果记录数，不是互相独立功能的数量。跳过不等于通过。记录丢失、自测未知状态或失败，会阻止绿色完成。文字使用 ASCII 英文，不依赖可选中文字体。

完整结束后自动恢复 A 策略。完成后打开退出菜单不会把已完成采集改判为中断。重复 L+R+Y 替换上次游戏 A/B 结果，但保留全部启动自测。屏幕提示的是“可以退出保存”，**不是“报告已经写入 SD”**：正常结束才导出 RAM 数据；强制关闭、崩溃或断电可能丢失尚未保存的报告。

## 为什么这次启动包含 100 个写表样本

上一版每个微基准样本单独创建 JIT，镜像写表与首次写入组出现不一致的性能结果。本版专门消除“切换这三种状态时重建 JIT”的混杂因素，一次运行同时检查寄存器、表地址、首次写入、边界检查和编译顺序。

五种模式：

| 模式 | 意义 |
| --- | --- |
| original_table | 原始 X24 页表路径 |
| same_table_x25 | X25 指向与 X24 完全相同的表，分离换寄存器与换表地址的影响 |
| mirror_write_table | 独立写表，全部开放 |
| first_write_table | 四个后备页重新布防，各首次写走回调，然后开放 |
| preopened_write_table | 在计时外走首次写回调开放页面，计时内不再布防 |

后三种模式在一组中使用**同一个 JIT 对象、同一个代码缓存、同一张写表和后备数据**，不会按样本销毁/重建 JIT。每个配置先在计时外预热，检查结果和实际回调数。

总计：两种边界检查策略 × 两种分配/初始编译顺序 × 五轮 Latin 顺序 × 五种模式 = **100 个样本**。每行都含五种模式，每个模式在五轮中出现在全部五个采样位置。每个样本执行 262,144 次 Guest store，结果写到 `store_matrix_v2` suite。

记录包括内层 wall ticks、查询包围区间的 thread CPU ticks、两个查询返回码、采样前后核心编号、实际回调数、读写表及后备地址、JIT 对象身份和 Guest PC。不改 CPU 频率、线程亲和性或优先级。最多三个 8 MiB code slice 同时存活；所有测试资源在 Guest 启动前释放，随后关闭空闲测试 code pool。

注意：JIT 对象地址**不是**发射出的 native 热块地址。CPU tick 区间包含查询开销，不能当成裸 store cycles；核心首尾一致也不能证明中途未迁核。ns/store 包含 Guest 循环及分派，不能直接乘游戏写入次数来预测帧率。边界检查关闭组只对应已知对齐的测试工作负载，不能替代最终生产路径。

## 保留的自动验证和游戏 A/B

原有 Heap 1/4/16/64 页 RO→RW、Stack alias、解除映射后权限和数据恢复、原生 A64 store、Dynarmic 8/16/32/64 位首次写、Run/Step、停止后重新布防、别名、非连续后备、跨页及无效映射检查继续自动执行。

`QueryMemory.type` 只按对外内存类型记录，不推断未导出的内部 capability 位。所有权限测试使用独立页和 RW guard。权限恢复/unmap 失败则保留 backing，禁止带着残留映射 chainload，避免释放仍不可访问或被借用的内存。本版不故意触发写保护异常。

游戏 A/B 不变：A 对 texture staging 执行原平台维护和 VMA flush；B 对 non-coherent staging 只省掉前一次重复平台维护，仍执行 VMA/Vulkan flush；coherent 映射保留原 workaround。没有修改 External Host Guest pointer、其他 ring buffer、GPU barrier、fence、图像布局转换或 staging 重用时机。

两组仍使用 `ordinary_texture_cache_existing_hash_footprint` 参考模式，关闭普通纹理缓存的同 scene memo 和旧 Hash64/unsafe skip。它不覆盖 surface cache、DoubleBuffer block memo 及现有 Hash 范围之外的一切一致性情况。不要把本版严格 Hash 下的速度与早先 24.36 Loop/s 视为同配置比较。

本轮**没有新增 GPU staging→image→readback 内容验证、生产环境并发重新布防或完整 HLE/GPU 写追踪**。游戏内真实写表仍未开启；不得把隔离测试通过当作生产 hashless tracker 已完成。GPU 可见性和实际画面正确性仍需独立验证。

## 报告与工具

仍只有一个 TraceLab 会话目录。`memory-validation.json` 保持兼容的 `schema_version=1`，新增 `validation_revision=2`、`capture_outcome` 和 overlay 元数据；既有 `tests` 和 `benchmark` 结构不变。八段的基本就绪门槛与既有分析器对齐：每段至少 100 个有效间隔、有效时长至少目标的 90%，没有 overflow，中断或测试记录丢失；B 必须有实际去重调用和字节。最终 PC 分析仍会检查其他日志的完整性和配对条件。

原来的 Node 分析器继续提供游戏性能、全部自测明细和 staging A/B：

```sh
node analyze-switch-tracelab.mjs "/path/to/session" --html memory-report.html > memory-report.json
```

产物增加标准库 Python 分析器，专门比较新的同 JIT 矩阵：

```sh
python3 analyze-switch-memory-v2.py "/path/to/session" > memory-matrix.json
```

它按边界检查和分配顺序分组，输出 Original→Same-X25、Same-X25→Mirror、Mirror→FirstWrite、Mirror→Preopened 的配对比值。检测失败、迁核、重复/缺失坐标、回调数错误、表或后备身份不一致时排除对应配对；五轮不全则不输出性能比值。CPU 查询不可用时保留有效 wall 数据，但 CPU 比值为 null。不会把所有模式和顺序混成一个“平均开销”。旧采集没有矩阵时显示不可用，不伪造样本。

通常只需提供整个会话目录，无须在 Switch 上或电脑上手动运行分析器。结合可见画面异常、当时的时钟、渲染和 LSFG 设置反馈；A/B 中途不要更改条件。

## 配置与开发检查

```ini
mode=auto
memory_validation=auto
validation_warmup_seconds=120
validation_settle_seconds=3
validation_phase_seconds=20
```

`memory_validation=on` 仅取消自动开始；`off` 关闭验证。`mode=off` 同时关闭 TraceLab。可调范围仍是预热 0–600 秒、稳定等待 1–30 秒、测量 5–120 秒。很短的测量配合低帧率可能达不到 100 间隔门槛，建议保留默认配置。

主机回归新增真实控制器+HUD 共用文本规则检查，覆盖暂停、重启、完成、低样本、失败、读线程并发、报告和退出菜单。矩阵驱动测试提取真实驱动函数，使用明确标注的 host fixture 验证顺序、JIT 复用和报告协议；**不模拟或证明真正的 Dynarmic A64 执行**。已有实际 emitter/Oaknut 字节检查保留在 CI，完整 Switch 编译检查目标平台类型和链接。最终仍需同一个 NRO 的实机结果。
