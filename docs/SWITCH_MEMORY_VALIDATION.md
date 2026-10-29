# Switch：GPU 镜像第 2 版——真实 shader/BDA 与链式交接

基线 `f5d441a3`（CI #52 成功），实机会话 `1793294897`，merge revision `ab5b634e60df`。继续 PR #15，不重做静态场景采样，不启用 Hash64 或其他概率跳过。

## 本轮已经得到的证据

设备报告 144 PASS / 8 SKIP / 0 FAIL（152 条结果记录）。原 GPU 镜像的 13 项内容控制及清理全部通过；15 次完整字节审计、11 次 CPU fault、28 次 fence 等待，所有权上传 65,536 B / 下载 12,288 B。测试从建立到结束累计 11,999,221 ticks / 19.2 MHz，约 0.625 秒，包含渲染回调之间的时间，不是裸 GPU 执行耗时。会话持续 84.378 秒；无需在 DONE 后继续运行。

这是 transfer/fill/image 路径的证据，不是 shader store、render attachment、多线程或真实游戏免 Hash 的证明。旧报告的导入统计在退出后已全部 released，因此 `imported=0` 不代表游戏没有使用导入。

## 下一版改动

仍为自动 14 步、相同私有 16 KiB CPU Heap 和独立 Vulkan 镜像。原 `gpu_fill` 控制改为真正的 compute shader 通过 Buffer Device Address 写入；CPU 局部覆盖、同值覆盖、GPU→CPU 获取等原有 oracle 继续检查完整内容。

图像控制前，在同一个 command buffer 中执行一组 shader 复制，涵盖完整范围、4 字节偏移、非工作组整数长度、跨页、最后一个 word 和零长度。每次刻意多发一个 workgroup，shader 必须在任何物理地址访问前检查 count。GPU buffer 前后各有 512 B 保护区，完整读回核对；保护区仍在实际分配范围内。

每次子范围复制前污染目标，回写前污染原镜像，避免漏执行命令但旧数据碰巧正确。整条 shader 链只有最终一次 fence 等待，阶段之间使用实际 compute/transfer 内存依赖；之后做 buffer→image→buffer 与独立 CPU oracle 对照。所有权仍只在完成后发布。异常处理器内不新增 GPU 等待、锁或分配。

测试构建使用 Vulkan 1.1 协商，以合法使用物理存储指针所需的 SPIR-V 1.3。shader 不要求 `shaderInt64`。这是明确的测试构建 API 合同变更；非 Switch / 非 TraceLab 构建不变。主机 glslang 编译和 spirv-val 验证在 CI 前置阶段执行；CMake 也按源文件依赖生成目标 shader，设备不编译 GLSL。产物带 shader 二进制、反汇编和来源/工具/摘要 manifest。

游戏的 Direct Heap、整映射按需导入、Hash/scene memo、上传与同步决策不变。没有给 Guest 内存加 RO、没有启用游戏免 Hash，也没有将测试范围当作任意游戏 shader 的寻址范围。当前只验证私有单 owner compute/transfer/image；attachment、IPC、异步 HLE、并发重新布防及生产资源生命周期仍未获得这套测试的覆盖。

## 一次上机

构建成功后替换产物根目录的 `Vita3K.nro`，启动游戏即可。无需固定场景、L+R+Y 或 30 秒等待。首帧之后自动运行；原有启动 CPU/JIT 控制早于 renderer，仍无启动前 overlay。

面板显示 `GPU MIRROR`、`Step x / 14`、`Compute BDA + image`、真实进度，以及审计 / dispatch / guard 计数和所有权传输量。只有实际完成、内容检查和清理通过才显示 `DONE - REPORT READY`；失败显示 `DONE - CHECK REPORT`。看到任意 DONE 即可正常退出，交一个完整 TraceLab ZIP，不需要为绿色状态重跑。

每项危险操作前仍写入并 fsync 故障检查点。异常退出保留检查点和 crash log，不删标记重复触发。未确认 fence 完成或 CPU 权限未恢复时，继续保留资源并阻止 chainload。

## 报告与验收

保留 `capture_plan=gpu-mirror-1` 的兼容封装，在 `guest_backing.gpu_mirror.probe_version=2` 明确标识新实验。v1 日志仍可分析，但不能通过 v2 shader 验收。

v2 要求一次成功的 `shader_program` 记录、至少 5 次 fill dispatch（含零长度负载）、5 次 copy dispatch、恰好一次链式提交，以及 GPU guard 审计；dispatch 总数必须与分类之和一致。旧逐项内容控制、cleanup 和 suite 仍必须各有一次成功记录。缺失/重复/失败、缺 shader 版本或证据都会拒绝通过。每项控制现在记录自身耗时；耗时含布防/清理/同步，不应当作纯 GPU 时间。

测试结束时冻结导入元数据，之后的导出表示测试完成瞬间，而不是销毁后的零值；后续生命周期错误仍保留。该快照早于游戏重场景，不是新的场景覆盖率统计。上传/下载量只计所有权交接，不包括所有审计读回或 shader 内部复制流量。

## 主机与目标检查

`test_switch_gpu_mirror.py` 保留 262,144 次所有权转换、真实控制器/HUD/报告及缺项否决测试，并调用共享 BDA ABI/range 测试。后者覆盖 8,192 个固定种子复制、每个保护字节和漏复制/漏回写/无边界判断负例。CPU 数组替身不是 SPIR-V 或 GPU 执行。

安装 devkitA64 时，前置检查使用真实 libnx/Vulkan-Hpp 编译生产后端。主机需 `glslang-tools` 与 `spirv-tools`（CI 已配置），缺工具或 shader 验证失败应直接失败，不能跳过验证冒充成功。
