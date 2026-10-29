# Switch 下一轮：真实 GPU 镜像、内容归属与图像读回

基线 `3b6baac6`（CI #50 成功），实机会话 `1793291653`，merge revision `faf303f4eec3`。沿用 PR #15，不再重复固定场景的整映射延迟导入统计。

## 本轮证据与路线决定

130.647 秒会话、2969 条帧记录，129 PASS / 8 SKIP / 0 FAIL；没有报告记录溢出。延迟导入确实避免了 3 个映射、6 MiB 的导入，无导入/后备错误。但目标窗口内 **558 个输入范围（34,946,544 个逻辑字节）首尾全部 DeviceShared**，无采样槽失败；全部位于 Guest `0x70348000` 开始的同一个 **96 MiB** 映射内。该映射同时用于 buffer 与 uniform/device-address，整段按需导入仍然固定了全部纹理输入。use mask 不证明 GPU 从未写入。

保留延迟导入，不为 Hash 目标再重复这一轮，也不擅自收窄任意 shader 寻址范围。下一步验证独立 CPU/GPU 表示之间的显式内容归属。游戏本身仍用 Direct Heap、整映射按需导入和原有 Hash/scene memo；**没有新增游戏 RO、Hash 跳过、全量 DoubleBuffer 或 GPU 同步省略**。

## 一次启动，自动 14 步

成功构建后替换产物根目录 `Vita3K.nro`，启动游戏即可。**无需进入固定重场景、无需 L+R+Y、无需等 30 秒。** 首个实际渲染回调先显示自动实验面板，后续每次回调完成一项控制。原有 CPU/JIT/混合权限自测仍执行；renderer 尚未建立的启动部分没有游戏 overlay。

面板显示 `GPU MIRROR | AUTOMATIC GPU TESTS`、`Step x / 14`、百分比、字节审计数、实际上传/所有权读回字节数。它按真实控制完成数更新，不靠时间推算 DONE。失败立即显示 `DONE - CHECK REPORT`；全部控制和资源清理通过显示 `DONE - REPORT READY`。任意 DONE 后正常退出，交一个完整 TraceLab ZIP，不为绿色状态重跑。

每项危险操作前保留原有 fsync 检查点。异常退出请保留 `heap-fault-<revision>.json` 及 crash log，不删除检查点重试。单个测试 fence 有 5 秒等待上限；无法确认完成的资源保留，禁止带未解决资源 chainload。5 秒是失败界限，不是运行时长承诺。

## 实际测试范围

仅独立的 16 KiB CPU Heap、独立 Vulkan 上传/GPU/读回 buffer 和 64×64 RGBA8 optimal image，不修改 Guest RAM。GPU buffer 不导入 CPU Heap；另行获得设备地址，保持 CPU 页可保护。缓存维护针对专属、完整映射的 non-coherent VkDeviceMemory；提交复用现有 general queue mutex，但私有 pool/command/fence 独立。先等 fence 并 invalidate，再进行 CPU 字节比较。

14 步包含初始上传、512 次 clean 复用、CPU 首字/末页字修改、CPU 只读时 GPU fill、GPU 最新后 CPU 局部覆盖、CPU 写回旧 shadow 相同数值、跨页写、CPU 脏后 GPU 局部覆盖、GPU→CPU 获取、buffer→image→buffer 全字节读回、同址新逻辑生命周期、最终核对和清理。每一步记录累计传输、fault、fence、审计数与首个不同字节位置/期望/实际值。

内容归属按页为 CPU/Both/GPU/Invalid。CPU 同值写也是写事件；部分覆盖先获取未被覆盖的旧内容，全页覆盖可丢弃旧副本。传输回调只有在 fence/CPU 可见性完成后才发布 Both。失败后不把未完成内容发布为有效。每步结束恢复 scratch RW 后才让出当前渲染回调；异常处理器不等待 GPU、不分配、不拿 mutex。

**这是单 owner 的显式获取协议。GPU 写来自真实 transfer/fill，不是 shader store 或 render attachment；image 也不是游戏帧的画面比对。** CPU 的获取发生在普通控制路径，不代表任意 Guest fault 已能安全等待 GPU。IPC、异步 HLE、多写入线程、生产级重新布防和全局资源生命周期仍需接入。仅验证 BDA 非零，不验证任意 shader BDA 寻址。

## 报告与验收

新 `capture_plan=gpu-mirror-1`，`guest_backing.gpu_mirror` 导出状态和累计计数，`tests` 保留逐项记录。分析器必须同时看到原有恢复/缓存控制、13 个 GPU 内容用例、cleanup 与 suite 完成，缺项、重复、失败、缺 fence/传输/独立字节审计证据都拒绝通过。不要求旧 30 秒场景或纹理采样数；私有测试不能产生游戏性能收益声明。

主机使用相同内容归属和控制驱动，GPU 后端明确为数组替身，包含丢上传/伪造读回的负例、262144 次固定种子 CPU/GPU 部分更新，以及真实控制器/HUD/导出/分析器。SDK 可用时，CI 在全项目编译前用实际 devkitA64/libnx/Vulkan-Hpp 编译真实 GPU 后端模板。主机通过不等于设备 Vulkan 执行。

接入延续 fourth whole-blob-pinned `switch-deferred-import.json`；源身份或锚点不匹配即拒绝，产物记录编译身份，无需手工应用。`mode=off`、legacy 与非 Direct Heap 路径不启用自动镜像实验。
