# Switch 写入审计 v4：一次运行，自动正反例 + 真实缓存变化对账

## 上轮的实际结论

`1793256692` 的四段 A/B/B/A 完整，167 条启动记录为 161 PASS / 6 SKIP。
A 合计 9.71990 Loop/s，B 合计 9.68555 Loop/s，B 约低 0.353%，平均每轮多 0.365 ms。
B 两段的 1,414,599 次观察中，1,413,626 次有已登记页覆盖（99.9312%），仅 3 页首次写入。
这表示本场景的增量观察开销小，不证明整个写表架构零成本。

**观察器 changed=0 并不等于游戏纹理没有变化。** TraceLab 在整场记录了 177 次纹理缓存
Hash 变化，130–220 秒窗口中还有 3 次，而写入观察器四段均为 0。
缓存项和共享 backing footprint 使用不同的历史，且 B 重新建立基线；也存在未覆盖路径。
旧日志缺少逐次关联，不能凭这些计数把原因归给 HLE/GPU，也不能据此取消 Hash。

## 只做一轮

替换产物根目录 `Vita3K.nro`，启动游戏；独立能力/JIT回归和新增的正反例自动完成。
到固定重场景按一次 **L+R+Y**，保持场景至四段结束（默认每段 3+20 秒，共约 92 秒）。
任意 **DONE** 都表示可以正常退出保存；给回一个完整 TraceLab 目录。
不需要切配置、多个 NRO，也不要求为了“0 次自然变化”再跑一次。
旧 `mode=off` 仍被遵守。启动测试仍在 renderer 创建前运行，首帧后 HUD 显示结果汇总。

## 自动测试不会修改游戏数据

新增 24 条 `observer_challenge` 结果，使用独立 scratch backing 和**生产 Table 观察器**。
设备适配器通过真实 Dynarmic `Run` 执行普通 store；主机测试的适配器不是原生 JIT。
测试含：像素写入、同值写入、页内相邻写后再次写像素、软件别名、独立 palette、跨页、
失败/成功的回调 CAS、重复发布 PTE、Hash 过程中收到写通知、映射变更及阶段变更。
另有六个完成通知 API 的测试和缓存变化对账测试。

故意绕过通知的 raw host 写入是**负对照**：预期出现 unknown 才算该项 PASS。
它不是游戏画面错误；这些计数在独立 Table 内，不混入真实游戏的写入统计。
测试签名是两个 32-bit scratch 值的无碰撞打包，不是 Vulkan 图案读回或完整纹理解码测试。
CAS 项测试的是真实回调 CAS，不声称覆盖 native LDREX/STREX、多核 reservation 或完整 GPU/IPC。

## 实际接入的写入通知

仅为 SceLibc 的 memcpy/memmove/memset 与 sceClibMemcpy/sceClibMemmove/sceClibMemset
六条路径增加**成功返回后的**范围通知。sceClibMemcpy_safe 经既有委托调用同一入口。
不替换原来的内存操作，不额外扫描内容，不把所有 Guest store 转为软件回调。
报告按六条 hook 分别计数，未调用就是未验证，不用零计数冒充全覆盖。

这些是完成通知，不是同步协议。写入可能在通知前与 Hash 竞争；IO、解码器、其它 HLE
和 GPU 写入仍未完全覆盖。GPU 写入完成、CPU cache invalidate、所有权转移没有在本轮改动。

## 修复和新的证据

* **同 PTE 重复发布不能解除监视。** 保留原有空写表项；只有真正的映射变化才更新镜像。
* **Hash token 绑定阶段与资源 mutation。** 已知写入或映射在 Hash 中变化时，丢弃观察结论；
  B→A 即使 epoch 数字不变，也不能接受上个阶段的 token。完整内容 Hash 仍返回给原缓存。
* **区分 exact byte hit 与 page-only hit。** 相邻写不是纹理字节修改证据，但首次 JIT 写会开放
  整页，因此该页所有依赖仍必须保守 taint。不能因第一笔没命中纹理字节就允许免 Hash。
* **逐条缓存失效对账。** 普通 full-hash 路径实际决定更新时，记录旧/新 Hash、地址、大小与
  最后一次观察的 receipt。区分新基线、观察器已见相同字节但缓存项历史不同、同时确认变化、
  已知竞态、无匹配 receipt。early-return 会清空旧 receipt，不能拿前一张纹理的结果作证。

`changed_cpu_seen`/`changed_hle_only` 是本 B epoch 的粘滞证据，不是每次变化的因果归属。
`clean_candidate_*` 是未见已知写入的理论候选计数，**不跳任何 Hash**。
`hash_skip_authority=false`、`arm_safepoint=false`、`HLE_GPU_coverage=false` 保持不变。

## HUD 与报告

四段进度与正常退出提示保留；`DONE - NO LIVE CHANGES` 为黄色，表示数据可交回，但没有
自然变化检验覆盖率。独立正反例不把它伪装成自然游戏变化。已知失败仍是红色。
出现 unknown、cache receipt mismatch 或 no changes，都先交报告，不盲目重跑。

`memory-validation.json` 保持顶层 schema 1，`validation_revision=4`，write_shadow schema 2。
分析器仍叫 `analyze-switch-memory-v3.py`，向后兼容 v3，并校验 v4 必需的 24 条正反例。
直接运行：

```sh
python3 analyze-switch-memory-v3.py /path/to/tracelab-session --html write-report.html > write-report.json
```

## 构建与验证边界

观察器、运行时、HUD、分析器和回归测试都是普通源码修改。六处实验接线沿用
`switch-live-write-watch.json` 的 whole-blob-pinned recipe，由构建前脚本校验和应用，
没有运行时源码下载。源码不匹配即停止构建；CMake 的 `--check-applied` 检查已展开结果。
产物包含 `write-watch-hooks.json`，可核对真正编译的接线文件。正式整合前应将这些实验
接线展开为普通源码修改。

本地主机测试和 sanitizer 不能替代 Switch 原生执行、GPU 可见性及完整 NRO 链接。
完整主机 preflight 和交叉构建由 CI 执行；实机性能需本轮日志，不预先承诺收益。
