# Switch 下一轮：Direct Heap 真实游戏接入 + 无扫描缓存控制

基线：`806c738e`，CI #46 成功；实机日志 `1793281108` 的 revision 为该 PR 的合并提交 `a2b33c51dae2`。

## 本轮日志结论

103 条记录：95 PASS、8 SKIP、0 FAIL。原生 8/16/32/64/128-bit store、完整寄存器对照、跨页写、host memset、真实 Dynarmic 直写与软件别名均通过。JIT 每个用例一次硬件 fault、零普通 MemoryWrite 回调。64 轮 arm/write/retry/restore 共 9,587 ticks，在 19.2 MHz 下平均约 7.802 us/轮；这不是裸异常延迟。

External Host 导入前 RO/RW 成功；导入中 `attr=0x4`、RO 返回 `0xd401`、仍为 RW；解除导入后恢复可改权限。不能将“CPU Heap 可保护”推广成“GPU 导入范围也可保护”。本次没有 GPU 写入/IPC/并发写入证明。

## 一次上机操作

构建成功后，只替换产物根目录 `Vita3K.nro`。启动游戏并正常操作/加载即可，无须 L+R+Y，也不需要固定重场景的 72 秒 ABBA。

启动保留已通过的快速回归。首次普通纹理 Hash 自动调用新的隔离控制，随后首个实际渲染循环开始 **30 秒真实 Direct Heap 检查窗口**。面板显示 `DIRECT HEAP | GAME VALIDATION`、进度、剩余时间、帧数、纹理输入范围数、后备错误和 GPU import 状态。

看到任意 `DONE` 后正常退出保存，提交一个完整 TraceLab 会话 ZIP。少于 120 个渲染循环、没有纹理输入、自动控制缺失或失败会明确标记，不要求为绿色结果盲目重跑。同一次采集即可提供部分有效证据。启动测试在 renderer 创建前没有逐项 overlay；首帧后显示进度。此窗口不是游戏性能 A/B，也不宣称画面正确性自动通过。

## 真实 Guest 改动

TraceLab 默认将原连续页池直接作为 Guest 的宿主 Heap 后备，不再为这些数据页保留 Stack 区域或调用 Heap→Stack svcMapMemory。4-GiB Guest 页表、连续 run 分配器、清零、反向地址映射、软件别名和已有 GPU mapping 路径保留。物理预算保留 512-MiB 最小、768–1280-MiB 偏好及至少 1-GiB 宿主 heap 预算，不再受 Stack 最大空洞约束。预算/分配失败会报告失败，不静默退回旧拓扑。

Null Guest 页使用空 PTE，不给 malloc 后备页留下 Perm_None。回收前检查 Heap/RW/无 DeviceShared、IPC 或设备引用；未满足时不回收、不强制解除驱动引用。最终释放前重新检查整个池，无法确认时保留后备而非 free。

**游戏内没有开启页面 RO、写异常追踪或新的免 Hash。原普通 Hash/scene-memo、Vulkan 同步及上传策略保持。** GPU 导入的 Guest 范围继续 RW；旧线性 PC fault handler 不会启用。

## 同一版本的自动无扫描控制

调用 renderer 实际 Hash 函数，在独立四页 Heap 中建立单一资源（12 KiB 像素 + 1 KiB palette）。在 RO 且未写期间复用摘要，不 memcmp、不复制、不再次扫描；首次写 fault 使其失效，重新布防/计算后建立新基线。

控制包括初始基线、512 次 clean 重用逐次独立审计、像素首/中/末变化、palette-only、跨页 store、裸 host memset、同值写入、同页非输入区域写入、同址新生命周期和计时后审计。故意破坏 dirty 证据的主机负例必须被识别为失败。另有私有 13-KiB 输入的 512 次读取 ABBA；独立审计不在计时内，结果仅表示这个隔离输入，不可外推为游戏 FPS。

该控制限于单 owner、已知生命周期、无 GPU/IPC、无并发生产者。它不解决全 JIT 安全点、并发 rearm、异步 HLE 输出或 GPU 所有权。

## 自动后备分类

对首次遇到的纹理像素/palette 输入做有界查询，区分普通 Heap、DeviceShared 和其他范围。最多 512 个槽、每次最多 8 次槽探测/256 个内核块；冲突或无法查询明确计数。采样结束后停止查询。

范围统计允许重叠重复计数，是首次观察时的布局快照，不是唯一物理容量、全量覆盖率、永久可保护授权或未来免 Hash 的证据。报告的分配/回收计数是进程累计，纹理采样/帧窗口是本次会话。

## 配置与退出

无需修改旧配置即可启用新默认；`mode=off` 或 `memory_validation=off` 仍关闭实验并保留旧拓扑。显式旧开发实验也不切 Direct Heap。仅排查时可设置 `guest_backing=stack_alias` 恢复原 Stack 映射；不要在运行中切换配置。

Scratch 故障测试继续保留每个危险阶段之前 fsync 的 `heap-fault-<revision>.json`。首次执行仍可能异常退出；保留 standalone checkpoint 与 crash log，不删除标记反复试。GPU 导入测试不提交 GPU 命令。无法确认 RW 恢复时保留 scratch 并禁止 chainload。

## 构建/测试

所有集成从精确的原 Git blob 展开。新增的有序 recipe 只能沿用同一个原始 blob pin；原文件或锚点不匹配时事务式拒绝构建，不作模糊替换。构建产物保存所有 recipe 的哈希与实际编译源码身份。

新增测试运行真实 C++ handler + Linux mprotect/重试（Horizon frame 为显式合成）、实际 XXH3、控制器/HUD/JSON/analyzer，并包括丢失 dirty 的负例；本地这不是 A64/Horizon/Vulkan 驱动执行。有 SDK 时先编译新增真实头文件与现有 C++/汇编 ABI gate，再构建完整 NRO。最终仍需目标 CI 与实机证据。
