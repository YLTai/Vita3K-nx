# Switch 内存验证 v4：判变逐次对账 + 精确范围持续观察

## 本轮依据（capture 1793256692）

v3 的四段 ABBA 完整结束，167 条启动记录为 161 PASS / 6 SKIP / 0 FAIL。
两个相邻 B/A 吞吐变化为 -0.300% 与 -0.408%，仅表示在共同诊断开销之上的增量。
第一段 B 的 8,191 个武装页没有首次写；第二段 B 的 8,237 个武装页有 3 个首次写。
两个 B 共 1,414,599 次 Hash 中，1,413,626 次属于登记覆盖（99.931%）。
但 observer 的可比较内容变化为零；同一会话的 TextureHashChanged 并不为零，
稳定窗口 130–215 秒内有 3 次。v3 没有实际缓存比较点的逐事件关联，不能推断差值原因。
三个写入地址各自确实有同起点的 4 字节 footprint，且同页还有 +4/+8/+12 的其它 footprint；
不能把它们全判为邻接假阳性，也不能把整页都视为内容已写。

因此不再要求重复相同静态场景以“证明零失配”。v4 自动制造正/负例，
同时对账真实 TextureCache 的判变，保留一次游戏采集回答性能与覆盖问题。

## 最少操作：仍然一个 NRO、一次约 92 秒采集

替换构建产物根目录 `Vita3K.nro`。启动的既有回归与新增 canary 自动执行。
进入同一重场景后按一次 **L + R + Y**；自动 A → B → B → A，
默认每段 3 秒稳定等待 + 20 秒测量，共约 92 秒，不含预热/启动。
出现任意 **DONE** 后正常退出保存，提交一个完整 TraceLab 目录。
不必为没有内容变化或黄色提示反复运行；这些同样是结果。
`mode=off` 仍被遵守；旧 `validation_experiment=write_shadow` 直接选择 v4 路径。

面板独立于 FPS 开关，以原有最多 4 Hz 更新，显示阶段、倒计时和退出提示。
新的 `Exact` 是与精确 footprint 重叠的资源命中计数，不是唯一页面数；
`HLE` 是带显式通知的资源命中；`Cache changes` 是会话内真实缓存判变数。
记录跨越菜单预热和重启，不要把它误认为仅 B 段计数。
无可比较 observer 变化时会给出黄色提示，完成后仍明确显示 **EXIT GAME NORMALLY TO SAVE**。

## B 的变化：只对选定页持续回调，不再“一次写入后开放”

普通页面仍然是既有页表直写，不增加所有 store 的计数器/bitmap 查询。
B 中已纳入监视的页持续保持写 PTE 为零，相关 store 才进入回调；
这保证同页第二次、第三次写入仍可被观察。
每次回调按实际宿主字节范围检查 pixel/palette 依赖，而不是给整页所有资源粘上 CPU 标记。
`page_only_stores` 单列同页但不重叠任何 footprint 的写入。
资源的 CPU/HLE 版本以**上一次可比较 Hash**为界，不把一段内曾经写过当成未来所有变化的解释。

若单个 B 段达到 100,000 次受监视 store，开放观察页并标记 `guard_budget_exhausted`。
完整 Hash 继续，但该 B 不再作为完整持续观察的性能样本，HUD 以 CHECK REPORT 提示。
这是显式成本保护，不是静默跳 Hash 或静默忽略失配。
同一正常 PTE 的重复发布也不能误开仍在观察的写 PTE。

六个明确范围的 HLE 输出入口增加操作完成后的通知：SceLibc 的 memcpy/memmove/memset，
以及 SceLibKernel 的 sceClibMemcpy/sceClibMemmove/sceClibMemset（safe 版本转调已有 memcpy）。
小范围非纹理输出只查询对应页，不遍历全部纹理；大范围查找由已登记页数限制。
这些是**事后通知**，不是 write lease、并发快照、IPC 或完整 HLE/GPU 追踪。
版本相关仅表示这个比较区间见过写入证据，不证明唯一因果；未观察到仍不能认定没有写入。

## 两个判变口径直接对账

`hash_texture_data` 仍无条件执行原完整 Hash。新增 `live_cache_decision` 位于
TextureCache 真正比较 previous/current Hash 的位置，分别记录：CPU 区间证据、
HLE 通知证据、未归因、未覆盖、observer 首次基线、observer 不同基线、
观察期间变化/映射失效、无关联观察、不支持的 Hash 路径。

`write_shadow.cache_ledger.changes` 等于九类原因之和；分析器进一步与
`summary.json` 的全会话 `texture_hash_changed` 核对。**分类不完整或总数不符不能报通过。**
缓存 Hash 的 xor-1 标记在对账时恢复成 raw Hash，以便与 observer 的值可比。
每条判变可保留 slot、Guest 地址、范围、tick、epoch、缓存前后 Hash 和 observer 前值。
仅在缓存判变时保存详细事件；所有比较的整数总数一直保留。
预热/非采集与正式采集分别保留最多 64 条详细记录；超限明确计数，禁止报告“零漏记”。

Token 增加映射/写入版本；Hash 期间重映射的旧 token 不得给新 backing 建立基线。
这些检查只覆盖已观察的元数据变化，不证明真实内存快照是原子的。

## 自动正反例（不修改游戏内存）

新增 9 项 `write_watch_canary` 在既有隔离 MemoryFixture 中运行：
真实 Dynarmic 标量写→真实 observer、重复别名写、CPU 后裸指针写的负例、
同页邻居写的负例、显式 host 通知正例、成功/失败 CAS 契约、
缓存判变但 observer 首次基线的对账、Hash 中途 remap、释放前全部恢复开放。

前两项由真正的 Dynarmic A32→A64 store 驱动；CAS 契约直接调用 observer 的原有 CAS primitive，
不是“native STREX 已验证”的声明。四字节测试直接以完整 uint32 值比较，无摘要碰撞问题。
故意绕过的写入必须被标为未归因，才算这个 canary PASS。
测试 Table 与游戏的 live Table 完全独立，反例不会污染真实游戏的未知变化计数。
启动测试仍早于 renderer，HUD 在游戏首帧显示结果汇总，不假装启动期已有图形面板。

## 分析与边界

使用同一个兼容 v3/v4 的脚本：

```sh
python3 scripts/analyze-switch-memory-v3.py /path/to/session --html report.html > report.json
```

v4 分析器验证 9 项自动 canary、四段时长/覆盖、变化分类、成本保护、
事件丢失和实际缓存总数对账。没有 summary 时明确提示对账未验证；
保留 v3 读取兼容，并对零可比较变化添加警告。

两组保留 FULL Hash、独立写表和同样跨页检查；A/B 只测选定页持续观察的增量，
不是相对无插桩 NRO 的全部开销，更不是免 Hash 提速。
本轮不去掉 callback 读取/CAS 的共同观察开销，避免同时改变多个性能变量。
不做硬件 fault 恢复、全 JIT safepoint、完整 HLE/GPU 所有权或 Vulkan round-trip 声明。
不因 `covered_checks` 高、没有未知事件或 canary PASS 启用 Hash 跳过。

## 构建与验证状态

本地已编写代码与可复现主机测试；本次会话没有 GitHub 写入接口或 Switch 交叉工具链。
补丁未推送，尚无 v4 CI/NRO，更没有 v4 实机结果。v3 的 CI #40 成功不代表 v4 成功。

实验接线配方扩展到五个精确 Git blob，仍在 CI 构建前自动应用。
本地补丁安装器会校验全部基准文件与五份配方输出；旧构建目录已有 v3 展开接线时，
仅当内容精确匹配旧配方才恢复原始源码，再安装新配方。遇到手工修改则停止，不覆盖。
不得以模糊替换或直接跳过接线校验来绕过构建问题。

主机测试验证真实 Table/API/控制器/HUD/导出/分析器；
canary driver 的主机执行使用明确标注的解释式 STR stub，不冒充 native A64；
native A64 发射和 SDK 链接由现有 CI gate 与下一次实机自动测试验证。
