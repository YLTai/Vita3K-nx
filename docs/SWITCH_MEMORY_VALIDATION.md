# 一次采集：完整 Hash、逐字节审计、逐字节比较

当前实验 `content-1` 在 PR #15 的实测提交 `58964c1e` 上继续修改，不回到已关闭的 PR #13。

## 为什么不再重复写入观察

实机日志 `1793268853`：六段完整，75 PASS / 7 SKIP，15 个隔离控制全通过；B 首次写与 C 持续观察相对 A 的吞吐变化约 -0.262% / -0.255%，而 A 首尾已漂移 -1.174%，不能把小差值当成稳定开销。正式采集未发生受监视纹理写入或可比较内容变化。213 次 TextureCache 判变出现在约 25–33、81、84 秒，早于约 95–167 秒的正式采集；全会话账本的 `missing_observation` 不是 B/C 漏写证明。

当前仍要逐次扫描内容，但不必在字节完全相同时反复执行 XXH3。下一步直接比较这个替代路径的正确性与净成本，而不是等待静态场景偶然写入。

## 最少操作

用构建产物根目录的 **Vita3K.nro** 替换原版本，进入固定重场景，按一次 **L + R + Y**。自动运行 **A/B/C/C/B/A**，每段 2 秒稳定等待、10 秒测量，合计约 **72 秒**。看到任何 DONE 后正常退出，提供一个 TraceLab 会话 ZIP。不要强杀进程；日志仍在正常退出时保存。

A = 原始完整 XXH3。B = 完整逐字节比较 + 每一次候选结果的独立完整 XXH3 审计，**总是返回独立参考 Hash**。C = 同一个逐字节比较器，只有字节完全相同才复用私有快照的 Hash。B 的额外审计开销不作为提速结果；主要比较 C/A。

不需要在阶段之间改配置。旧 `compact` / `write_shadow` 配置在此构建中自动进入新的默认比较实验；明确 `mode=off` 或 `memory_validation=off` 仍关闭诊断。仅开发回归需要 `validation_experiment=write_audit` 回到旧实验。不要为这次测试重跑 100 样本写表微基准或 staging A/B。

## 每次比较的内容与边界

范围与普通 `hash_texture_data` 相同：完整已有像素 footprint 和 P4/P8 独立 palette。不是采样、不是每 N 帧检查、不是“页没脏就跳过”。任意未通知的 CPU/HLE 写入，只要对当前 CPU 可见，仍会由每次完整字节比较发现。小于 4 KiB 或超出容量的输入退回原 Hash。

命中时执行完整 `memcmp(source, snapshot)`，不复制。发现变化或首次使用时复制输入，**对实际复制出来的私有字节计算 Hash**，避免把旧源 Hash 与新快照错误配对。palette Hash 与像素 Hash 保持原 XOR 规则；缓存槽的 XOR-1 标记不变。地址重用必须重新比较内容，不信任地址身份。

快照字节预算 64 MiB、最多 1024 个条目，LRU 在分配前淘汰，元数据另计。本次日志 509 个 footprint 的逻辑大小合计约 32.75 MiB，预算不是已证明足够所有游戏的保证。每次切换阶段清空快照，冷启动代价位于稳定等待；退出/完成清空字节存储。分配失败退回原 Hash并停止当前比较。

B 中任何不一致都返回原始完整 Hash，禁用本会话的候选路径并尽早显示 DONE - CHECK REPORT。它可能是算法/接线问题，也可能是两次读取之间的并发写入，报告不伪称已经定位根因。不要为了获得绿色而重跑。

这是字节 Hash 计算的优化实验，不是完整脏页架构。CPU/GPU 可见性、Guest 映射生命周期、GPU 私有图像、并发写入和 surface-cache 一致性仍依赖现有协议。比较器与原 Hash 读取相同的 CPU 映射，不新增或删除 Vulkan 同步。重复扫描仍存在，并且比较需要读取 source 和 shadow 两条数据流，可能受带宽限制甚至更慢；必须看同版本 C/A 实测。

## 默认关闭游戏内写入观察器

此实验的 A/B/C 都使用原游戏 JIT 页表路径，不绑定额外写表、不启用额外跨页检查，也不对 callback read / exclusive write 加入观察器 mutex 或登记。既有 JIT 本身的回调、原子操作与调度语义不变。这消除了上版各组共同存在的观察器开销；不能把新 A 与旧 A 的差值归功于 memcmp，必须比较新版本内部 C/A。

场景 Hash memo 仍仅在正式采集时禁用，导航与完成后恢复旧行为。所有 unsafe interval skip 保持关闭。`strict_hash` 配置含义在这里是“禁用场景 memo 的完整内容参考”，报告另外明确 `full_hash_every_call=false`、`full_byte_validation=true` 和 `exact_byte_equality_only`。

## 自动控制与 UI

保留已经通过的 15 个观察器隔离控制作为回归。第一次普通纹理 Hash 时，渲染线程使用实际 `hash_data/XXH3` 自动跑另外 12 项私有内存控制：首中尾字节变化、palette、同址重用、span key、LRU/容量回退、每次命中审计、复制后源改变、快照与 Hash 绑定、碰撞仍需比较字节、清理。输入是独立 scratch，不改游戏 RAM。

控制缺失/失败时禁止开始采集。HUD 显示 Equal / Audits / Diff / Shadow MiB、当前 A/B/C、剩余时间及保存提示。无自然内容变化不再要求重复测试；缓存相同内容本来就是 C 的工作负载。发生不一致或分配失败会尽早停止，显示原 Hash 已恢复；已完成后打开退出菜单不损坏结果。

## 输出与分析

仍是同一个 `memory-validation.json`，新增 `content_compare`：六段真实 begin/end tick、输入字节、相等比较、刷新复制、digest 次数/字节、逐次审计、不一致、淘汰、峰值内存、首次差异。计数包括 settle，性能时间排除 settle。逻辑字节量不是测得的 DRAM 带宽。

唯一分析入口：

```sh
python3 analyze-switch-capture.py tracelab-session.zip --html report.html > report.json
```

分析器同时支持旧 compact-1。新模式检查完整六段、至少 50 个测量间隔、90% 有效测量时长、所有自动控制、B 每个候选结果都审计、零差异、C 实际有相等命中、各计数对账及预算。失败不输出收益比率。两份对称样本仍只是描述性比较，不是统计显著性证明；尾延迟、A 首尾漂移同样保留。

## 构建与验证

保持单 core + 单内嵌 core 的 launcher。普通纹理接线继续使用原先 whole-blob 固定配方，新增比较器/controller/analyzer 为普通跟踪源码。CI 在完整 checkout 应用配方并验证，用户不用应用补丁。

主机测试编译真实比较器、控制器、HUD、导出和分析器；完整 checkout 时使用固定 xxHash 子模块，局部环境可以使用系统 XXH3并明确报告。设备上的实际 Hash 控制和真实帧耗时必须由这个 NRO 执行，主机结果不冒充实机结果。
