# 并发模型加固调研：抢占、分派、共享状态与连接密度

> 日期：2026-10-02
> 状态：调研完成，待评审（Spec：`.trae/specs/concurrency-hardening/`）
> 参考源码：`../../thrid-sdk/`（go-go1.27.1、otp-OTP-29.1.1、loom-fibers、nginx-release-1.31.6、brpc-1.18.0、libhv-1.3.4）
> 方法：所有外部结论附 文件:行号；lumin 现状结论附仓库内行号，关键证据已人工二次复核。

## 1. 背景

app 模块（ServiceApplication/WebApplication）已具备多 worker（SO_REUSEPORT）× 多协程（每连接一个）× reactor 事件驱动的网络服务能力，并经 4000 长连、file.recv 50×10MB × WORKERS=4 等实测。与 Java/Go 对比后识别出四个短板：

1. CPU 密集任务在 C 内置执行期间不可抢占；
2. worker 间靠内核 5 元组 hash 分派，可能不均，连接不迁移；
3. 跨 worker 共享业务状态需用户自行同步；
4. 每连接栈偏重（实测约 74 KiB RSS）、connCapacity 需手配、实测口径仅 4000 长连。

调研目的：用外部成熟实现的源码证据校准方向，区分"真短板"与"共同纪律"，给出可落地、不过度设计的方案序列，避免无效改造。

## 2. lumin 现状事实链

### 2.1 协程与调度（已具备的基础设施超出预期）

- 有栈协程：fcontext 切换（`kit/runtime/src/lm_ctx_jump_x86_64.S`），mmap 栈 + per-thread 栈池 + 末页 `PROT_NONE` guard，两档 SMALL=16 KiB / NORMAL=128 KiB（`kit/runtime/include/lm_co.h:239-251`，`kit/runtime/src/lm_stack_pool.c:123-145`）。mmap 惰性分页，不触碰的页不占 RSS。
- reduction 预算：`LM_SCHED_REDS=4000`（`lm_co.h:174-175`），resume 时重装（`lm_co.c:408`）；扣减宏 `LM_BUMP_REDS` 挂在 **JMP 家族 / CALL / BUILTIN / MKCLOSURE / RETURN 等派发点**（`src/ir/vm_exec.c:393-437,450,483`）。
- tight loop 可抢占：纯表达式 `while` 每轮必经回边 OPC_JMP 扣减，约 4000 回边后经 `lm_co_slice_bump`（`lm_co.c:501-509`）→ can_preempt hook 校验四操作数栈深度 == entry_sp（`src/ir/vm_co.c:196-208`）→ yield，重入 FIFO 队尾。
- work-stealing 已存在：每 scheduler 一个 Chase-Lev WSQ（容量 4096）+ mutex 就绪队列 + 全局溢出队列，随机起点质数步长批量窃取 ≤32（`lm_scheduler.c:411-427,466-508`）；中立（非 pinned、stealable）协程可跨线程迁移，pinned 协程永不入 WSQ。
- sysmon 守护线程：schedtick 停滞 >10ms 时对**非 pinned**协程 CAS 写 `migrate_sched` 迁往 compute 池（`lm_sysmon.c:77-95`），compute 池只挂 scheduler 不挂 reactor（`lm_compute.c:60`）。
- blocking 池：阻塞文件 IO 已流放独立线程池并 yield（`lm_file.c` 各 `*_blocking` → `lm_blocking_submit`，`lm_blocking_pool.c:153`）。
- `preempt_flag` 字段仅两处 `atomic_init`（`lm_co.c:258,303`），**全仓无置位/读取点——信号式异步抢占通道是死字段**。
- `LM_BUMP_ALL_REDS` 宏已定义（`lm_co.h:202-205`），**全仓零调用点**；`vm_exec.c:424` 注释自述"长内建应主动 LM_BUMP_ALL_REDS 强制让出"，但无人遵守。

### 2.2 C 内置执行期间：当前抢占模型的最大盲区（短板 1）

- OPC_BUILTIN 在进入 C 函数**之前**扣 1 个 red（`vm_exec.c:426`），C 函数内部无任何 `lm_co_yield/LM_BUMP*` 调用。json 解析/序列化、qsort、大字符串/容器操作等纯 CPU 长 C 路径一旦进入，同 worker 上其他协程（含连接 IO 协程）全部等到它返回。
- sysmon 的 `migrate_sched` 只写下一让出点标志，对正在 C 内运行的协程同样无效；pinned 的 IO 协程连迁移资格都没有。
- 连跑超 50ms 仅告警不干预（`lm_scheduler.c:576-584`，LONG_SCHED_MS）。

### 2.3 reactor 与 accept（短板 2 的现状）

- per-thread 单 reactor：Linux epoll **边沿触发**，只用 ADD/DEL（等待时 ADD、唤醒后 DEL、再等再 ADD），无 EPOLLONESHOT / EPOLL_CTL_MOD（`lm_reactor.c:104-131`，全仓 grep 确认）；macOS kqueue EV_CLEAR。
- 连接按 fd 索引预分配池（默认容量 65536）+ `fd_map` 注册主人映射 + generation 代际守卫（`lm_reactor.c:327-374`）。
- self-pipe 跨线程唤醒（`lm_reactor.c:488-514,567-574`）；主循环已有 posted_accept / posted_events 双队列（`lm_reactor.c:57,615-624`）；定时器外置 TimerThread。
- accept：`listen` 不设 SO_REUSEPORT（`lm_socket.c:800-825`），由 lumin 层四参 ServerSocket 在 bind 前 `setOption("reusePort")`（`Socket.lm:84-96`，`lm_socket.c:1234-1243`）；每 worker 独立 listen 同端口，内核 hash 分派；accept EAGAIN → `co_wait_fd`（`lm_socket.c:827-877`）；reactor 层另有 accept4 到 EAGAIN 的 helper（`lm_reactor.c:630-677`）。
- reactor 指针经 TLS 取用（`lm_socket.c:128`），IO 等待强制 `co->pinned=1`（`lm_socket.c:144-154,249-256`）。**fd 的连接池索引、handler、co 唤醒全部依赖"reactor 单线程拥有"不变量。**

### 2.4 多线程共享状态（短板 3 背后藏着两个框架级真问题）

GC 本身是多线程安全的：`g_gc_mutex` + 入 GC 原子 CAS + 协作式 STW（线程/协程/CFrame 根注册表）+ 增量标记写屏障 + TLA 无锁本地分配链（`gc_runtime.c:90-274,2126,2237-2338`）。但：

- **【真问题 P0-A】全局变量根帧跨 worker 无锁共享，且读取路径会触发 realloc。**
  `StackFrame* g_main_root_frame` 是进程级裸指针，注释自述"所有线程共享此指针"（`src/ir/vm_exec_var.c:110-114`）。`LOAD_GLOBAL` 调 `frame_ensure_slots(root, idx+1)`（`vm_exec_var.c:136`），容量不足时对 vals/int_slots/flt_slots/ptr_slots/type_tags/refs **六个数组 realloc**（`vm_exec_var.c:84-105`），无任何锁。STORE_GLOBAL 写同一批槽位同样无锁。多 worker 下这是确凿的内存破坏竞态（不是"用户需注意"级别，而是框架允许的常规写法就会中招）。
- **【真问题 P0-B】qsort 比较器宽度上下文不是线程隔离的。**
  `src/ir/vm_builtin.c:730` 的 `static int g_cmp_width` 被两处排序路径写（`vm_builtin.c:2470,2473`）、比较器读（`:734,:745`），跨线程并发 sort 互相踩踏。同功能的 `g_sort_numeric` 在 `kit/runtime/src/lm_array.c:13` **已经是 `_Thread_local` 并带注释"多线程 sort 互不干扰"**——同一缺陷修了一半，g_cmp_width 是漏网项。
- **【真问题 P0-C】GC STW 安全点在纯 CPU 循环中覆盖不足。**
  `gc_runtime.h:146` 注释声称"VM 解释循环每条指令前调用 gc_stw_check"，但 src/ 内 grep 不到解释器调用点；VM 线程实际只在 gc_alloc、锁路径（`lm_lock.c:32`）、reactor 主循环（`lm_reactor.c:596`）到达安全点。多 worker 下纯计算协程不分配、不上锁时，GC STW 只能自旋等它到下一个回边/调用点，停顿不可控。

进程级共享/无锁状态全量清单见调研代理报告，除上述外值得注意的还有 `g_dns_timeout_ms`（配置型只读，benign）。

### 2.5 栈密度与容量旋钮（短板 4）

- mmap 已惰性分页（不碰不占 RSS），**不需要做稀疏提交改造**；实测约 74 KiB/连接的 RSS 是 VM 调用帧实际触碰的页（注释：accept loop → createHandler → ctor → spawn 每层 C 帧较大，64 KiB 会被顶满 SIGBUS，`lm_co.h:235-239`），SMALL 16 KiB 档因此尚未敢用于 VM 协程（`lm_co.h:241-245` 明确"待栈深 profiling 验证后另立"）。
- connCapacity 需用户显式与 ulimit 对齐（已有 `__private_system__fd_limit` 探测与告警，没有自动对齐）。

## 3. 外部源码对照

### 3.1 Go 1.27：双轨抢占与连续栈

- **协作抢占折叠进函数序言的栈边界检查**：编译器在每个函数序言插入栈下界比较；抢占时把 `stackguard0` 毒化为 `stackPreempt`，下次函数调用进入 morestack/newstack 后识别并转 `gopreempt_m`，摊还成本≈0（`src/runtime/stack.go:1122-1174`）。
- **信号异步抢占只救无调用死循环**：sysmon 检测同一 P 10ms 无 schedtick（`proc.go:6677-6716`，`forcePreemptNS=10ms`）→ tgkill 发 SIGURG → 信号处理器改写 PC 假装调用 `asyncPreempt`，汇编桩保存全部寄存器后进调度器（`signal_unix.go:74,375-385`，`os_linux.go:579-581`，`preempt_amd64.s:8-31`）。
- **信号方案不能抢占非 Go 代码（C 调用）**：安全点白名单严格排除 runtime 内部、原子序列、nosplit 帧、非 Go 代码（`preempt.go:443-483`，`signal_unix.go:447-458`）。即 Go 的信号抢占对 lumin 的"C 内置长调用"盲区同样无效。
- **连续栈**：初始 2 KiB、2 倍增长、GC 期使用率 <1/4 减半（`stack.go:77-89,1177-1208,1313-1334`）；拷贝时逐帧按**编译器生成的精确 GC 栈地图**改写栈内指针（`stack.go:762-767,984-1007`），源码直接注释"只有具备精确栈地图时缩栈才安全"（`stack.go:1241-1249`）。**lumin 是保守 GC，连续栈拷贝在原理上排除**；分段栈与保守扫描兼容但有 Go 已弃用的 hot-split 缺陷，工程性价比低，列为兜底而非选项。

### 3.2 Erlang/OTP 29：与 lumin 同构的 reduction 模型（直接参考）

- 预算常量 **CONTEXT_REDS=4000**（`erts/emulator/beam/erl_vm.h:53`，旧资料的 2000 已过时），与 lumin 相同。
- 解释器在每条指令 dispatch 处 `FCALLS--`，归零 `goto context_switch`（`emu/macros.tab:153-165`）；**JIT 模式改为仅函数入口与回边检查**（`jit/x86/instr_common.cpp:3235-3237`）——佐证 lumin"回边+调用点扣减"的选择正确，无需每指令扣。
- **长 BIF 用 trap 续体分片**：如 `length/1` 单次迭代上限 = `剩余reds × 16`，做不完把续跑状态存入寄存器、重定向 PC 到 trap BIF、重入队后从断点继续（`erl_bif_guard.c:280-300`，`bif.h:329-334`）。
- **CPU 密集 NIF 走 dirty 调度器池**：独立运行队列 + shadow process，一次性跑完不做 red 检查（`erl_nif.c:3464-3469,443-457`，`erl_nfunc_sched.c:137-144`）——lumin 的 blocking/compute 池已是同构设施。

### 3.3 nginx：accept 均衡的三种形态

- accept_mutex：所有 worker 共享 listenfd，同时刻仅持锁者把 listener 挂自己 epoll，锁 500ms 粒度轮转（`src/event/ngx_event.c:219-239,649-652`）；`ngx_accept_disabled = connection_n/8 - free_connection_n`，忙 worker 逐轮退让（`src/event/ngx_event_accept.c:139-140`）。
- reuseport：每 worker 独立 socket，**TCP 无 BPF 定制分派**（全树 sk_data 零命中，BPF 仅 QUIC），纯内核 hash——即 lumin 现状。
- EPOLLEXCLUSIVE 一次只唤醒一个 worker 但内核偏向最先注册者，nginx 每 accept 16 次 del+add 轮换等待队列位置修补（`ngx_event_accept.c:454-493`）。

### 3.4 brpc：单 acceptor + fd 哈希 + IO/计算两层解耦

- 每端口一个 Acceptor（reuse_port 默认关，`src/butil/endpoint.cpp:46`）；accept 后 `fmix32(fd) % dispatcher_num` 选固定 EventDispatcher（`src/brpc/event_dispatcher.cpp:78-85`）。
- **跨线程 epoll_ctl 是常规操作**（在 acceptor 线程对目标 dispatcher 的 epfd ADD，`event_dispatcher_epoll.cpp:160-172`）：内核允许，但前提是连接对象/回调/唤醒不依赖线程归属——brpc 的 Socket 放全局 SocketMap 按 id 寻址，bthread 不持有 reactor 指针。
- IO 线程固定、处理 bthread 可被 work-stealing 到任意 pthread（`tcp_transport.cpp:68-104`，`task_control.cpp:625-664`）。**这层解耦 lumin 不具备（TLS reactor + pinned），照搬等于重写网络栈，列为非目标。**

### 3.5 libhv：与目标方案同构的 detach/post/attach

- 官方示例 `examples/multi-thread/one-acceptor-multi-workers.c`：1 个 accept loop + N worker loop，acceptor 静态 RR 选 worker，`hio_detach` → `hloop_post_event`（mutex 队列 + eventfd 唤醒，线程安全）→ worker 线程内 `hio_attach` 后才首次 `hio_read` 触发 EPOLL_CTL_ADD（`event/hloop.c:276-312,788-826,835-860`）。
- **connfd 在 worker ADD 之前不注册进任何 epoll，全程零跨线程 epoll_ctl、零双注册窗口**；关闭也坚持 post 回 owner 线程（`hio_close_async`，`hloop.c:883-897`）。这与 lumin 的 reactor 单线程拥有不变量完全兼容。

### 3.6 Loom（旁证）

虚拟线程 pin 在 native/监视器帧内时让出失败，退化策略就是阻塞载体线程（`VirtualThread.java:877-923`）——与 lumin"IO 协程 pinned、阻塞活流放 blocking 池"语义一致。freeze/thaw 依赖精确 OopMap（`continuationFreezeThaw.cpp`），保守 GC 下不可借鉴。

## 4. 方案决策

### 4.1 P0：先修三个框架级正确性问题

| 项 | 决策 | 理由 |
|---|---|---|
| P0-A 全局根帧竞态 | 两步：① main 帧槽位数在编译完成后即确定，工作线程启动前**一次性按最终全局变量数定稿容量**，消除运行期 realloc 路径；② 跨 worker 对**同一**全局变量的读写竞争定义为用户责任（必须用 Concurrent* 容器/锁），写入多 worker 编程指南 | 不同变量槽位的稳定存储并发访问本就该安全；同一变量的 check-then-act 任何语言都不替用户保证（Go map/Java HashMap 同理）。读写锁包住每条全局访问热路径代价过大且不解决复合操作原子性 |
| P0-B g_cmp_width | 改 `_Thread_local`，与 g_sort_numeric 对齐 | 一行修复，同功能既有先例 |
| P0-C STW 安全点 | 在回边（JMP 家族，reds 检查同位置）加 `gc_stw_check_fast`；与 reds 检查共用回边，无直线无限代码 | GC 侧机制完备，只缺 VM 通道的到达点；回边是 tight loop 唯一无限路径 |

P0-A 容量定稿的实施前提（main 帧何时创建、全局槽数何时可知）在实施时先取证；若无法在启动前定稿，退化为"帧槽数组一次性大预留 + 禁止 worker 启动后 realloc（超限走显式扩容锁）"。

### 4.2 P1：C 内置抢占（OTP 模型，不做信号抢占）

- **新增 C 侧检查点原语**：`lm_co_builtin_checkpoint()`，在长内置的循环回边调用。语义：reds 未耗尽直接返回；耗尽时在"内置安全深度"执行协程 yield，resume 后**沿原 C 栈继续**（有栈协程天然保留局部变量与循环计数器，不需要 OTP trap 续体那样保存迭代状态）。
- **扩展 can_preempt hook**：vm_co 侧增加"内置模式"——进入 OPC_BUILTIN 时记录四操作数栈深度 `builtin_entry_sp[4]` 与 `in_builtin` 标记；hook 规则从"必须等于 entry_sp"放宽为"内置模式下必须等于 builtin_entry_sp（内置期间栈深度不变）"。内置作者的契约：checkpoint 处不得持有指向操作数栈内容的裸指针（与 SSO 默认串修复沉淀的同一纪律）。
- **前置门**：OPC_BUILTIN 派发前若 reds 已耗尽，先 yield 再进 C（避免预算 0 进入长内置还要等内部第一个 checkpoint）。
- 首批试点内置：json parse/dump（大文档循环）、sort（比较器内 checkpoint）。建立模式后其余内置按基准数据补，不搞一次性全量插桩。
- **明确非目标**：信号异步抢占（Go 自己都不能抢 C 代码，对本盲区无效，且需安全点元数据/信号栈/跨平台处理，成本高收益低）；qsort 之外的第三方不可重入 C 调用不保证可抢占。

### 4.3 P1：单 acceptor + 应用层 RR 分派（可选模式，默认不变）

- 新增 dispatch 模式，**reuseport 保持默认**（向后兼容、零迁移成本）；RR 模式采用 libhv detach/post/attach 范式：
  1. 单 acceptor（复用现有 accept4 到 EAGAIN 逻辑）只做 syscall，不跑业务；
  2. `worker = (next++) % N`，把 `{fd, sockaddr}` 投入目标 reactor 的**新增 MPSC 新连接队列**，复用 self-pipe 唤醒（`lm_reactor_wakeup` 已有）；
  3. worker 在 posted 处理点批量 drain：**在自己线程内** get_connection → ADD(EPOLLIN|ET) → spawn handler，TLS reactor/pinned 语义完全不变；
  4. connfd 在首次 ADD 前不挂任何 epoll，零跨线程 epoll_ctl；
  5. 容量拒收检查从 accept 点移到 worker drain 点（拒收在 owner 线程 close）。
- **背压**（借 nginx accept_disabled）：worker 上报在役连接数/队列水位，acceptor 对高压 worker 跳过（跳到最空闲者或短轮询退避）；所有 worker 队列满则停止 accept，ET 下内核自然反压，不无限缓存 fd。
- 非目标：连接热迁移（Netty/libhv/nginx 同样不迁移）；brpc 式 IO 与计算两层解耦。

### 4.4 P2：栈密度与容量自动化

1. **栈高水位采样先行（数据驱动，不拍脑袋调默认值）**：在协程 yield/销毁点从 fcontext sp 与 stack_base 计算剩余量，经 schedStats 暴露 p50/p99/max；先在 file.recv/echo/HTTP 三类负载取样。
2. 按数据把**浅栈路径的 spawn 点显式标 SMALL**（如纯转发 IO 协程），提供栈档 API；HTTP handler 等深 VM 帧路径维持 NORMAL。默认值不改，避免 SIGBUS 风险。
3. **connCapacity 自动对齐**：用户未显式设置时，默认 per-worker = min(fd_rlimit - 安全余量, 上限)；显式设置仍优先。
4. **万级长连基准**：ulimit 调优后 1 万/2 万长连、workers=核数，记录错误率、RSS/连接、worker 分布，数据归档本文档。
5. 明确不做：连续栈拷贝（保守 GC 原理性排除）、分段栈（hot-split，兜底保留）、稀疏提交（mmap 已惰性）。

## 5. 非目标汇总

- 信号式异步抢占（SIGURG/PC 改写/安全点元数据）；
- 连续栈拷贝、分段栈、协程栈热迁移；
- 连接诞生后的跨 worker 热迁移、brpc 式 IO/计算两层架构重构；
- 替代用户做共享业务状态的同步纪律（不引入 STM/actor），不做 race detector；
- 全局变量读写锁化（容量定稿 + 文档化分工即可）。

## 6. 实施路线图

- **P0（正确性）**：T1 g_cmp_width TLS → T2 全局根帧容量定稿 → T3 回边 GC 安全点；
- **P1（并发能力）**：T4 内置 checkpoint + hook 内置模式 + json/sort 试点；T5 acceptor RR 模式 + 背压；
- **P2（密度/易用性）**：T6 栈高水位采样 + SMALL 试点；T7 connCapacity 自动对齐；T8 万级长连基准。

每片独立可验证、可提交；T1/T3 互不依赖可先做。全部片完成后以 TSAN 构建（竞争类 AC）+ ASAN 构建（内存类 AC）+ 正常构建（性能回归）三套口径验收。

## 7. 风险与验证策略

- hook 放宽内置模式是最敏感改动（file.recv 抢占污染刚修复）：必须坚持"内置期间四栈深度不变才放行"，ASAN 全量 + 20 万次 checkpoint 压力；
- RR 模式 fd 交接窗口：靠"ADD 前不注册 + owner 线程 close"两条不变量保证，专项 fd 号循环复用测试；
- 全局帧定稿：先写最小多 worker 竞态复现（TSAN 必报），修复后同口径转净才算闭环；
- 性能：reds/GC 检查共用回边，热路径只增一次分支，验收 QPS 回归 ≤5%。
