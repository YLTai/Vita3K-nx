# Switch 下一轮：GXM 同步输出范围与观察器元数据成本

从实测 `da085fef`（CI #57 成功；merge revision `97cea41baa472`）线性继续 PR #15。仍只用一个 NRO、一轮自适应 ABBA 和原生进度面板；不再增加权限或私有 GPU 探针。**完整普通纹理 Hash 仍是渲染更新依据，未启用新的游戏 Hash 跳过。**

## 日志 1793346806：等待分段有效，不等于全部写源已覆盖

157.475 秒会话，2958 条连续渲染循环记录，无记录溢出；75 PASS / 7 SKIP / 0 FAIL。四段均自动取得50个有效间隔，测量分别为6.617、6.764、6.776、6.560秒，没有达到延长上限。合并 A=7.588732、B=7.385330 Loop/s，B吞吐低2.680%，约增加3.629ms/loop；这只是共享插桩下选定页观察的增量，不能当作总追踪成本或免Hash收益。本轮没有受监视纹理store。

两段B的378047次已覆盖检查均通过JIT发布检查。377195次CPU版本候选中，56616次通过同步HLE范围与GPU提交历史检查（约15.01%）；139422次被未知在途调用排除，181157次被未知写入历史排除。未报告候选Hash反例。38152次原生等待park/resume平衡。

剩余采样阻塞包含sceGxmDraw、fragment/vertex uniform与texture设置、sceAudiodecDecode。归因是全会话1/64采样的重叠目击，不是各段独立份额、耗时或写入证明。GPU提交历史稳定不等于GPU完成；异步HLE/IPC仍未覆盖。

## 本版代码

### 批量接入14个已审核GXM入口

七个仅更新context的uniform/texture/stream setter登记真实`SceGxmContext`范围。七个可能构造命令的program/viewport/visibility/draw入口，只对immediate context登记context及**完整Guest VDM命令环**。命令并不总在宿主池：实际allocator可在Guest VDM范围内placement-new并连接命令，所以不能只登记context。

通过实际`command_allocator_size * sizeof(renderer::Command)`计算命令区，验证乘法、Guest起止地址、宿主指针及范围溢出；条件不成立就保持未知。可能调用Guest内存回调的deferred命令路径不收窄，`sceGxmDrawPrecomputed`等未审核外层也不能借助嵌套已审核helper改名放行。

这只是**调用线程的同步前端输出**，不覆盖renderer异步处理、shader写入、render target、GPU完成或其他外部写源。不改变命令内容、队列顺序、shader边界、barrier/fence或上传方式。音频解码不能用写完之后才assert的`pcm_size_max`作为事前上界，因此保持未知，而不是为了候选数加入只读名单。

### 降低已有观察器的元数据查询成本

NID索引增加固定1024槽提示缓存，命中仍比较完整NID；冲突回退原256行有界表。归因容量、未知调用和失败处理不变。

输出范围缓存只记“这组三个输出范围与当前纹理兴趣集无交集”。它不缓存内容、纹理Hash或复用许可。新增像素/palette范围、清空或重新登记都会推进兴趣集版本；输出尚在进行时新登记的重叠资源立即busy，结束时正确解除。命中的输出区间绝不按负缓存省略版本/busy更新。同值写入仍是事件。

这些优化减少元数据锁内的查表与范围扫描，不删除同步，也不增加普通store的全局计数或bitmap查询。固定缓存开销有界。实机收益尚待本轮测量。

### 同轮统计候选字节量

新增`epoch_audit.candidate_work`：按四段导出完整Hash、CPU候选、HLE范围候选及提交历史候选的**累计逻辑输入字节**，包含palette。相邻集合必须满足包含关系，并与已有Hash字节账本对上。

新增`range_contracts.metadata_cache`：NID命中/探测次数、无交集命中/实际范围扫描次数及兴趣集版本。进入/退出的处理总数要与scope数量核对。分析器继续支持旧日志，不为旧记录猜测候选字节。新字段声明`content_reuse_authority=false`。

逻辑字节含重复输入与稳定等待，不是唯一容量、DRAM流量或已省下的Hash时间。缺字段、错版本、丢段、计数不一致或虚假复用授权都被拒绝。旧“没有粗粒度候选”的提示不再掩盖已经存在的范围候选。

## 实机操作保持不变

成功构建后替换产物根目录`Vita3K.nro`。进入相同目标场景，按一次 **L+R+Y**。自动执行 **A→B→B→A**；每段稳定1秒，测量至少6秒且50个有效间隔，最多12秒测量预算。正常回调预算约28～52秒，停顿可增加墙钟时间。

原有overlay继续显示 **OUTPUT / WAIT AUDIT**、实际段号/进度/剩余预算、`Samples:n/50`与`EXTENDING`，以及HLE/unknown/overlap/diff。没有渲染回调就不凭计时宣布DONE；暂停仍显式中断，完成后退出菜单不破坏结果。

**看到任意DONE后正常退出，交一个完整TraceLab ZIP即可。** 黄色、零候选、反例或样本上限都先交报告，不重跑同版去追求绿色；不增加第二个NRO或手动配置切换。导航和采集结束保留既有scene memo。

## 验证范围

新增主机回归对真实范围表与修改前固定Git blob作40000步随机语义对照，覆盖NID冲突/归因饱和、活跃输出期间新增palette、重置、同值事件与未知前缀。GXM helper测试覆盖context/完整VDM/不同context类型/非法或溢出环/未知外层。真实observer验证候选字节，直接编译生产JSON新增片段并检查分析器正反例。

普通与ASan/UBSan通过；已有发布域和HLE范围纯逻辑、精确写入观察回归通过。本地是由源码备份重建的稀疏工作区，GXM context使用明确的主机替身；没有把它说成完整仓库、Horizon/JIT/GPU或NRO验证。CI先扩展全部固定blob配方并运行`--require-source`和完整旧/新preflight，再编译链接目标。

**本轮没有live Guest RO、Hash64、新Hash跳过、强制DoubleBuffer或额外GPU等待。** 零反例不是异步写入覆盖证明；生产复用所需的GPU完成、异步输出和资源生命周期仍是单独条件。
