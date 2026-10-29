# Switch 下一轮：按实际 buffer 需求延迟 GPU 导入

基线 `1c6825d5`（CI #49 成功），实机日志 `1793288984`，合并 revision `fba5466a64b3`。继续 PR #15；不改变旧实验分支，不重做快照比较或写表性能矩阵。

## 这轮已经得出的结论

120.434 秒会话、2721 个连续渲染循环、129 PASS / 8 SKIP / 0 FAIL。16 种四页脏页组合全部恢复成功；缓存像素、palette、跨页、memset、同值、同址新生命周期及独立审计全部通过，最终 `safe=1`。混合 R/RW 的分段恢复缺陷已获得设备验证。

实际场景窗口 30.047 秒、727 次 render 回调，768 MiB Direct Heap 后备错误为零。登记的 **466 个纹理输入范围、31.081 MiB 逻辑输入全部为 DeviceShared**。24404 次有界槽查找失败不是 24404 个独立资源；范围重叠也不能相加成为独立物理容量。这足以说明仅换 Direct Heap 不能直接给当前主要输入启用 RO。

私有四页缓存的 512-read ABBA 只测单 owner、无 GPU/IPC 的微型输入；不代表游戏提速。GPU 导入期间 RO 返回 0xD401 是已执行的能力边界，不能当成漏测或尝试强制清除 DeviceShared。

## 本次真实架构改动

在 TraceLab + Direct Heap + External Host 模式中，把逻辑 GXM 映射和实际 Vulkan 导入分开：

- GXM map 时登记范围、宿主指针和唯一生命周期 token，保留逻辑 `mapped_memories` 项；不创建 VkBuffer、VkDeviceMemory 或 GPU 地址，不复制内容。
- buffer/transfer 绑定、shader device-address 查询或 CPU-buffer 同步第一次真正需要时，调用原来的导入实现。**一次导入完整逻辑映射**，保留任意 shader 寻址的上下界、原缓存维护和原 DoubleBuffer 失败回退。
- 已创建映射继续直接使用；每种新增用途只更新一次登记，不在每 draw 加登记 mutex。
- 未导入范围也持有后备生命周期引用。包含任意 pending/imported 范围或别名的后备不能提前回收。首次使用重新核对当前页表和完整连续后备；指针不一致、空 GPU 地址或创建失败不会发布为有效映射。
- unmap 和 cleanup 沿用既有等待及资源销毁顺序，之后解除生命周期引用。不会在运行中迁移、解除已使用的 GPU buffer 或收窄 shader 范围。

仅用作 CPU 纹理上传源的逻辑映射，可能全程无需导入。若纹理和 buffer 共处同一大映射，首次 buffer 使用仍使整段导入；本版如实记录这一限制，不用窄页导入冒充已经解决任意寻址。

**没有对游戏页设 RO，没有新增免 Hash，没有更改 Vulkan barrier/fence 或上传决策。** 普通 Hash/scene memo 保留。注册表 mutex 不用于异常处理器，也不构成新的 renderer 并发协议。

## 一次上机

成功构建后替换产物根目录 `Vita3K.nro` 并启动游戏。旧配置无 `gpu_import` 时默认使用本次 deferred；不要求修改配置。

自动 scratch 控制通过后，面板显示 `WAITING FOR SCENE`。进入目标实际场景，按一次 **L + R + Y**，运行 **30 秒**。窗口中可按正常游戏方式操作，不需切换模式或重启。任意 `DONE` 后正常退出并提交一个完整 TraceLab ZIP。

如果启动控制失败或生命周期错误，直接在 `DONE - CHECK REPORT` 退出，不必耗满窗口。不要为了绿色状态重复。异常退出保留检查点和 crash log；不删除故障标记反复重试。pre-render 自测仍早于游戏 overlay；首帧之后显示进度。

Overlay 新增 `Deferred / GPU / Avoided / Errors`：pending/imported 的逻辑映射 MiB、未使用即解除的映射数及错误数。逻辑映射可能重叠，这些数不是唯一物理内存，更不是帧率收益。

## 同一日志回答的问题

输入采样表增为 2048 槽、最多 32 次探测；失败次数明确导出。重复输入只计数，不重复查权限。每个新范围最多查询 256 个内核块；超出或失败的部分算 other。

结束时只做一次有界的权限元数据复查，不读取纹理内容。每个输入导出首次和结束时 plain/DeviceShared/other 分类，结束时冻结导入注册表，避免退出销毁对象后把曾经使用的映射误报为未导入。查询是顺序时间局部采样，不是原子快照或未来可保护授权。

注册表最多保存 1024 个历史记录；耗尽时新 map 使用原 eager 路径并报告 capacity fallback，不丢失尚存引用、不把缺失记录当作从未使用。记录包含 Guest/host 范围、token、pending/imported/fallback/released 状态、实际用途及一次 materialization 时间。shader-write 标志表示潜在用途，不声称确实发生 GPU 写入。

主要决策：有多少被反复 Hash 的范围可保持普通 Heap？哪些因为整段映射混用而仍然被导入？首次实际导入是否暴露寻址、同步或生命周期问题？本轮不是新旧性能 A/B；不能将其 Loop/s 与严格 Hash 实验直接比较。

## 开关与验证

`gpu_import=eager` 仅用于排障；不要求同一轮手工 A/B。`mode=off`、`memory_validation=off`、legacy 模式或 Stack 后备仍走原逻辑，不在运行中改配置。

主机回归使用真实注册表、真实 renderer wrapper 和调用入口，Vulkan materializer 明确为替身，覆盖需求才创建、重复绑定、内嵌映射、4 GiB 边界、GPU 地址上下界、同址新生命周期、别名回收阻止、后备替换拒绝、创建失败、fallback、容量与冻结报告。主机线程测试仅验证注册表，不是并发 Vulkan 或设备验证。

完整 checkout 的 CI 从实际展开的 renderer.cpp 提取调用入口，并检查释放和 CPU-sync 接入；稀疏本地环境只对已审查原文和固定配方执行相同入口测试。原 handler/POSIX fault、XXH3 正反例、HUD/JSON/analyzer、导入控制流、ABI/A64 汇编以及 ASan/UBSan 继续回归。完整目标编译、链接和实机游戏分别验收。

集成由第四份 whole-blob 固定配方 `switch-deferred-import.json` 完成，与前三份顺序组合；任何原始身份或锚点不匹配均事务式拒绝。产物记录编译身份，无需用户手工应用补丁。
