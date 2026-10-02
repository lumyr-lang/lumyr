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

## 8. T5 实施取证与详细设计（2026-10）

### 8.1 现状事实链（文件:行号）

- **accept 在 lumin 层，不在 C 层**：`lm_reactor_accept_one`（lm_reactor.c:630）
  全仓零调用；实际路径是 lumin accept 协程循环调 `socket.accept()` 内建
  （`lumyr_socket_accept`，lm_socket.c:827）：EAGAIN 时 `co_wait_fd(listener_fd,
  读)` 挂起协程，事件就绪 resume 后重试，每次只 accept 一个并 wrap 成 SocketObj
  GC 对象（`lumyr_socket_from_fd`）返回 lumin，由 AcceptLoop 调 `onAccepted`
  spawn handler 协程（ServiceApplication.lm:170-254）。
- **多 worker 现状**：ServiceWorker.run（ServiceApplication.lm:407-450）各线程
  自建 reactor+scheduler（TLS），各自 `ServerSocket(host,port,backlog,1)` 置
  SO_REUSEPORT 同端口 listen（Socket.lm:84-96），内核 hash 分派；workerList
  保留 acceptTotal 供分布统计。
- **reactor 主循环钩子点**：lm_reactor.c:589-624，process_events →
  posted_accept → posted_events；self-pipe `lm_reactor_wakeup`（:567）与
  ready_drain 钩子（:579）均为现成跨线程唤醒机制。
- **跨线程唤醒协程范式**：timer 线程已在用 `lm_scheduler_wakeup(sched, co)`
  （post 入 mutex 定向队列 + self-pipe，lm_scheduler.c:367），fired-before-yield
  由 fd 等待字三态仲裁（lm_socket.c:156-360）与 scheduler 入队语义覆盖。
- **reactor 引用计数**：refcnt 释放制（lm_reactor.c:530-548），跨线程持指针
  必须 retain/release——注册表 by_idx 取指针须在锁内 retain。
- **scheduler 注册表 g_scheds[128]**（lm_scheduler.c:41）是同构参考，但 RR
  投递目标是 reactor 而非 sched（worker drain 在 reactor 线程，fd 直接入
  reactor 队列），故单独建 reactor 注册表，显式按 worker idx 占槽。

### 8.2 C 层设计

1. **每 reactor 入站 MPSC 队列**（lm_reactor.c）：mutex + 单链表节点
   `{fd, kind, addr, addrlen, next}` + 节点 freelist；`inbound_pending`
   持锁计数；`inbound_cap` 有界（默认 4096，env `LM_INBOUND_CAP` 覆盖供测试）；
   `_Atomic int inbound_load` 由 owner 线程发布在役连接数。
   - `lm_reactor_submit_fd`：满返回 -1；入队后若有 arm 的等待协程则取走 waiter
     （一次性）并 `lm_scheduler_wakeup`，无需额外 wakeup（未 arm 说明 owner
     正在 pop 循环里，必将自行取到）。
   - `lm_reactor_inbound_pop`（owner 线程）/`lm_reactor_inbound_arm`
     （锁内复查队列：非空返回"勿等"，空则登记 `(co,sched)` 等待者）。
   - reactor 销毁时 drain 残余节点全部 close（fd 守恒兜底）。
2. **reactor 注册表**：`lm_reactor_register(r, idx)`（显式 worker 槽位）/
   unregister/count/by_idx_retained（锁内 retain）；注销先于最终 release，
   杜绝"取到即将释放的 reactor"。
3. **两个内建原语**（实现放 lm_socket.c，复用 co_wait_fd/包装函数）：
   - `accept_rr(listener, n, maxConn)`：先按 `(inbound_load, inbound_pending)`
     负载感知从 RR 游标选合格 worker（load<maxConn 且 pending<cap）；全忙→
     返回 1（不 accept，ET 反压）；注册表未满 n→返回 2；然后 accept（EAGAIN
     走 co_wait_fd），提交；竞态满员则换下一 worker，皆失败 close fd 返回 1；
     EMFILE/ENFILE→-1 退避码，致命→-3。
   - `recv_inbound(impl)`：owner 线程 receiver 协程用——pop 即 wrap 成
     SocketObj 返回；空则 arm + yield，被 submit 唤醒后重试。
   - 不变量：fd 在 owner 首次 ADD 前不挂任何 epoll/kqueue；拒收 close 全部
     在 owner 线程 receiver 内执行。

### 8.3 lumin 层设计（ServiceApplication.lm）

- 配置 `dispatch`（"reuseport" 默认 / "rr"）+ `setDispatch(mode)`。
- RR 模式：worker 0 建**唯一** listener（reusePort=0）并跑 RrAcceptLoop；
  **所有** worker（含 0）跑 RrRecvLoop（recv_inbound → onAccepted）；
  reactor 创建后显式 register(impl, idx)，acceptor 等 registry_count==n 开闸。
- onAccepted/markFinished 后 `publishLoad(impl, activeCount)`；容量判定保持
  在 worker drain 点（onAccepted 满员走既有 onRejectConnection，owner 线程 close）。
- 监督重启/错误分级复用 AcceptLoop 既有策略（RR 循环单独实现，避免抽象耦合）。
- WebApplication.run 仅 super.run()，RR 自动生效，无需改动（T5-d 仅验证）。

### 8.4 验证设计

- VM 枚举内 `tests/rr_dispatch_test.lm`：4 worker RR 长连 echo 全对；
  acceptTotal 偏差 ≤15%；小 maxConnections 压满后 backlog 排队→释放后补入
  无死锁；停机后统计在役归零。
- probe：tests/probe/rr_dispatch_server.lm + rr_drive.py，2000 长连
  lsof 双采样（fd 回落 ±5%）、容量打满、echo 100%。
- reuseport 默认模式回归（reuseport/conn_capacity/stress_concurrent/
  stream_reactor）+ 全量枚举 90+ 用例；ASAN 双模式零报告（含 fd 快速复用）；
  RR vs reuseport echo QPS 对比（≥3 次中位数，回归 ≤5%）。

## 9. T6 实施取证与详细设计（协程栈高水位采样 + SMALL 档试点，2026-10）

### 9.1 现状事实链（文件:行号）

- **栈档与分配已就位**：`LM_STACK_SMALL`=16KiB / `LM_STACK_NORMAL`=128KiB
  （`kit/runtime/include/lm_co.h:274-275`），`lm_co_spawn` 按 stack_size 推断档
  （`lm_co.c:229-230`），`lm_co_spawn_class` 显式选档（`lm_co.c:282`）；per-thread
  栈池两桶分档复用（`lm_stack_pool.c:157/193`）。
- **栈几何**（`lm_stack_pool.h:26-36`）：mmap 区间 `[mmap_base, mmap_base+total)`，
  `stack_base`=可用区末（高地址，栈从此向低生长），`stack_top`=可用区起（低地址），
  `stack_size`=可用区大小（不含 guard page）。注释口误修正：`stack_base` 是
  可用区末，紧邻其上的 guard page 为 PROT_NONE（`lm_co.c:543-544` 的
  "stack_base 指向 guard page 起点"表述不准——scanTop 下退一字只是避开
  紧邻 guard 的边界读取，不是 stack_base 落在 guard 内）。
- **栈使用量精确采样点 = `lm_co_yield`**（`lm_co.c:530-575`）：fcontext 后端
  `lm_ctx_jump` 把 callee-saved 现场 push 到协程栈，`ctx.sp` 指向保存区最低点
  ——即**当前已用栈的最低地址**；栈向低生长，故
  `highWaterBytes = (char*)co->stack_base - (char*)co->ctx.sp`，语义与
  `lm_co.c:543-555` 既有 GC 水位参考点完全一致（该点注册 scanTop=
  stack_base-sizeof(void*) 与 ctx.sp 间接槽）。yield 是唯一"栈冻结 + 现场已落栈"
  的点；resume 返回侧（`lm_co.c:439` 之后）看到的是同一冻结状态，等价采样。
  ucontext 回退后端 ctx 内嵌 ucontext_t 无 sp 槽，本设计仅对 fcontext 采样，
  ucontext 构建降级为只统计 spawn/destroy 档级计数（`#ifdef LM_CTX_FCONTEXT`）。
- **采样点取舍**：spec 要求 yield/销毁两点。销毁点（`lm_co_destroy`）不可靠——
  DEAD 协程的 ctx.sp 停在 `co_trampoline` 末尾切回点（接近空栈），不能反映
  运行期高水位；且协程可被在 SUSPENDED 态强毁。故以 **yield 点每事件采样**为
  主（覆盖全生命周期所有冻结态），`spawn/destroy` 仅做档级计数，不做水位采样。
- **schedStats 现有形态**：全局原子计数 `g_lm_sched_stats`（`lm_sched_stats.h:34-47`，
  live_co/force_yield/long_sched/stuck + pending_time 12 对数桶直方图）；
  trace 线程（`lm_sched_stats.c:160-192`）`LM_SCHED_DEBUG=trace:N` 周期打印
  全局行 + per-scheduler 行；lumin 门面 `LumyrThread/SchedStats.lm` 经
  `__private_system__sched_stats`（BUILTIN_SCHED_STATS，`vm_builtin.c:4388`）
  输出快照 map（buckets/bounds/分位数 lumin 侧算，`SchedStats.lm:82-98`）。
- **lumin 层 spawn 链路**：`Coroutine` 构造器（`LumyrNetWork/Reactor.lm:123-130`）
  → 全局内建 `spawn(f, arg)`（BUILTIN_CO_SPAWN，`ir_compile.c:321`）
  → `vm_co_spawn`（`vm_co.c:342-357`，硬编码 stack_size=0 → 恒 NORMAL 档）
  → `lm_co_spawn`。当前全仓**无任何** SMALL 档使用方（grep 零命中
  `lm_co_spawn_class`/`LM_STACK_CLASS_SMALL` 于 src/ 与 lumyr-lms/）。
- **压测入口**：echo = `tests/rr_dispatch_test.lm`（4 worker 800 长连）/
  `tests/probe/rr_dispatch_server.lm`（19510）；file.recv =
  `tests/probe/stream_reactor_server.lm`（19210，reuseport 大文件上传）；
  HTTP = `tests/probe/p1_header_min.lm`（HTTP 请求最小解析路径）。
  采样输出走 stderr trace 行（LM_SCHED_DEBUG=trace:N）+ SchedStats.summary()。

### 9.2 设计

1. **高水位采样**（`lm_co.c` yield 点，`#ifdef LM_CTX_FCONTEXT`）：
   `lm_ctx_jump` 返回后（resume 回来栈仍冻结态已过——采样必须在 jump **前**，
   即现场已保存但尚未切走时读 ctx.sp 无效，因 sp 是 jump 内更新的）。
   **正确采样点**：fcontext 下 `lm_ctx_jump(&co->ctx, &co->resume_ctx)` 把
   保存后 sp 写入 `co->ctx.sp`——该写入发生在 jump 内部、控制流转出前；
   因此采样应放在 **resume 返回侧**（`lm_co_resume` 中 `lm_ctx_jump` 返回后，
   `lm_co.c:439` 之后）：此刻 `co->ctx.sp` 已是冻结 sp，协程栈内容稳定
   （state 尚未被本线程以外触碰，SUSPENDED 补写也在此后），读取安全。
   与 yield 点语义等价（同一冻结状态），且不侵入 yield 的 GC 注册临界区。
   DEAD 协程（trampoline 末尾切回）采样为近零，用 state==DEAD 跳过，
   避免拉低分档基线；swap_out 路径（reaper 读 ctx.sp 拷栈）同理是冻结态，
   但 reaper 属低频维护路径，不重复采样。
2. **分档直方图**（`lm_sched_stats`）：新增 `stack_hw_buckets[2][N]`——
   第一维 = `co->stack_class`（SMALL/NORMAL 槽位 0/1），桶按**已用字节对数**
   分档（1K/2K/4K/8K/16K/32K/64K/128K + 溢出桶，共 9 桶，128K 桶上界即
   NORMAL 档限），另加 per-class `stack_hw_max` 原子 max（fetch_max 循环）。
   累计语义（不清零，与 live_co 同生命周期）——高水位是容量规划指标，
   要的是进程历史峰值分布，不是窗口值。
3. **输出**：trace 全局行追加 `stackHw[class] n/p50/p99/max`；
   `SchedStats.lm` 快照 map 增加 `stackHwSmall`/`stackHwNormal`
   （{buckets,boundsKib,count,max}），`summary()` 追加两档 p50/p99/max 摘要。
4. **SMALL 档试点**：依据 9.3 采样数据选定 spawn 点，扩展 `Coroutine` 构造器
   可选栈档参数（默认 NORMAL 零行为变化），经 `vm_co_spawn` 传至
   `lm_co_spawn` 的 stack_size（≤LM_STACK_SMALL → SMALL 桶），不改
   `lm_co_spawn_class` 的 C API（保持 VM 协程单入口）。预期目标：纯转发
   echo handler 协程；深 VM 帧路径（HTTP 解析/构造器链）维持 NORMAL。
5. **性能纪律**：采样 = 一次减法 + 桶判定（≤9 次比较）+ 两次 relaxed 原子加
   + max 的 CAS 循环，仅在 resume 返回路径（已有 CAS 与记账逻辑同点），
   不新增锁、不新增 TLS 查找（co 已在手）。

### 9.3 验证设计

- 三类负载跑 LM_SCHED_DEBUG=trace:500，归档 trace 行（p50/p99/max 必须
  ≤ 档上限：SMALL 档 max ≤16KiB，NORMAL 档 max ≤128KiB）；
- SMALL 试点后全量枚举 + ASAN 零报告（重点：无 guard page SIGSEGV/SIGBUS）；
- 热路径开销：trace 关闭时采样仍计（原子加），与 pending 记账同量级，
  QPS 回归沿用既有口径抽测。

## 10. Task 7：connCapacity 自动对齐 fd rlimit（实施取证与设计）

### 10.1 现状取证

- 容量语义：`lm_reactor_new(conn_capacity)` 按 fd 索引预分配两个 calloc 数组
  （`connections` + `fd_map`，每槽位百字节量级），fd 超容量直接拒绝
  （`lm_reactor.c:285-289`）。容量是**每 worker** 值：单线程在
  `onStart` 建 reactor，多线程在每个 `ServiceWorker.run` 建 reactor，
  均调 `app.connCapacity()`（`ServiceApplication.lm:531/1078`）。
- 取值链（改前）：`connCapacity()` = 编程式 `setConnCapacity` > 配置表键
  `connCapacity` > 硬编码默认 65536。**默认值与实际 rlimit 完全无关**：
  典型开发机 `ulimit -n` 仅 256 时照样按 65536 预分配（白耗内存），
  反过来低 rlimit 下服务能跟踪的连接数被 fd 卡死。
- 已有半成品：`checkFdBudget()`（run 入口调用，reactor 建立前）仅在
  cap > fdLimit 时双语 warn，**不自动改值**；内置
  `__private_system__fd_limit()` 走 `getrlimit(RLIMIT_NOFILE)`，
  约定 >0=软上限，-1=RLIM_INFINITY/查询失败/平台不支持（三态合一）。
- 既有测试 `tests/conn_capacity_test.lm` 断言"默认 == 65536"，语义随
  本任务改变，需同步改为与 `calcAutoConnCapacity(真实 fdLimit)` 对齐。

### 10.2 设计

1. **优先级**（显式优先，与 spec 一致）：编程式 `setConnCapacity` >
   配置表键 `connCapacity`（配置文件也是用户显式意图）> 自动对齐。
   自动路径用实例字段惰性缓存（`autoCapResolved/autoCapVal`）——
   `onStart` 先于 `run()` 的 `checkFdBudget` 执行，不能靠 run 入口预算；
   缓存同时保证多 worker 下只探测/告警一次。
2. **公式**：`per-worker cap = min(65536, max(1, fdLimit - 256))`。
   - 余量 256：stdio(3) + 每 worker listen/kqueue/self-pipe + 日志文件 +
     出站 fd + 库内部分配，256 覆盖常见部署且与 nginx 保守余量同量级；
   - 上限 65536：沿用历史默认，即自动对齐只"往下收"，不会悄悄放大
     内存预分配；
   - 极低 rlimit（fdLimit-256 < 64）：改取 `min(64, max(1, fdLimit-8))`
     保底（64 是最小可用池；reactor 容量 ≤0 会被 C 层重置为 65536，
     必须显式给出 ≥1 值）。
3. **探测返回三态分立**（小改内置，语义不再含糊）：>0=软上限；
   **-2=RLIM_INFINITY（无上限 → 直接取上限 65536，不告警）**；
   -1=查询失败/平台不支持 → 保守回退 1024 + 双语告警。旧调用方
   `checkFdBudget` 判 `fdLimit > 0`，两种负值天然跳过，兼容。
4. **可测性**：决策抽成**纯静态函数**
   `ServiceApplication.calcAutoConnCapacity(fdLimit)`，
   单测注入 fdLimit 覆盖高/低/极低/无上限/失败五态，不依赖改进程 rlimit；
   告警文案抽成纯函数 `fdLimitWarnMsg(fdLimit)`，单测断言中英双语标记
   （`/` 分隔 + 英文 "ulimit" 片段）。
5. **自动对齐结果打印一次双语 info**（含 fdLimit 与取定值），为 T8
   万级长连基准留口径；checkFdBudget 保留，此后只可能对显式值告警。

### 10.3 验证设计

- 扩展 `tests/conn_capacity_test.lm`：五态纯函数断言 + 显式（编程式/
  配置表）优先断言 + 真实 fdLimit 端到端取值一致断言 + 双语文案断言；
  原 echo 功能/多 worker 两个场景保留不动；
- TR-7.2：全量枚举（正常 + ASAN），零新增 FAIL。

## 11. Task 8：10000 长连容量基准（实测归档）

### 11.1 环境与方法

- 机器：MacBookPro12,1（2015 13" MBP），**2 物理核 / 4 逻辑核，8GB 内存**；
  未改任何系统参数：`kern.maxfiles=30720`、`kern.maxfilesperproc=10240`、
  **`kern.ipc.somaxconn=128`（listen backlog 4096 被内核截断到 128）**。
- nofile 调优：仅进程软上限 `ulimit -n 20000`（无需 root，未触硬限）。
  同机压测 fd 账：服务端 10000（listen+10000 连接）+ 客户端 10000 ≈ 20002，
  系统总量 30720 内；两端各自软上限 20000 内。
- 配置：RR 分派、**workers=2（=物理核数）**、per-worker maxConnections=5500
  （总额度 11000）、acceptQueue=true；**connCapacity 未显式配置，Task 7
  自动对齐给出 per-worker=19744**（20000-256），可容纳进程级 fd 号到 ~10015。
- 方法（`tests/probe/c10k_server.lm` + `c10k_driver.py` +
  `run_c10k_bench.sh`，归档 `/tmp/t8_bench/`）：asyncio 驱动 BATCH=100
  错峰建 10000 长连 + 建连即逐条 echo 校验；t≈25s 对全部存活连接做一轮
  全量 ping（轻量收发，响应带编号校验）；t≈65s 全关。服务端自适应
  打印 BASELINE / 每 2s PROGRESS（峰值 active）/ DRAINED；编排 ps+lsof
  三点采 RSS 与 TCP fd；驱动 ping 完成打标记文件，编排按标记采 ACTIVE，
  不赌墙钟。

### 11.2 实测结果（2026-10-02，TR-8.1 达标）

| 指标 | 结果 |
|---|---|
| 建连成功率 | **10000/10000 = 100%，bad=0 err=0**（somaxconn=128 未调，零 reset） |
| 建连耗时/速率 | 10.10s，**990 conn/s**（BATCH=100 错峰，单调爬升无突发丢失） |
| 保活全量 ping | **10000/10000 零错误**，一轮 17.92s（≈558 msg/s） |
| 关闭耗时 | 10000 连 1.47s |
| worker 分布 | acceptDist=activeDist=**[5000, 5000]，偏差 0%** |
| fd 守恒 | BASELINE fd_tcp=1 → ACTIVE **10001**（listen+10000）→ DRAINED **1** |
| RSS | BASELINE 10MB → ACTIVE **667MB（65.7KB/连接）** → DRAINED 834MB* |
| Task 7 联动 | 自动对齐 19744/worker，启动双语 info 一行，全程零容量拒绝 |

\* DRAINED RSS 834MB 高于 ACTIVE：ACTIVE 采样于建连+ping 完成点，之后
全量 ping 与 10000 协程退出/GC 销毁继续触碰 VM 帧与 C 栈物理页；本运行时
栈池 per-worker 缓存 8 个 NORMAL 栈且 macOS 不主动向 OS 回收空闲物理页，
故进程 RSS 不回落**不构成泄漏证据**——判定口径以 fd 归零（fd_tcp 10001→1）
与 active=0 为准（与 T5 三点 fd 采样结论一致）；OS 物理页在进程退出时归还。

观测项（非错误，不临时改设计——spec Task 8 明确要求）：
1. **GC 协程注册表全局锁是万级吞吐瓶颈**（T5 已记录）：建连期 990 conn/s
   尚可，但全量 ping 仅 ~558 msg/s——每条 ping 触发的协程 resume/yield
   都挂摘全局注册表，2 worker 真并行下锁竞争把吞吐压到单核量级。
2. 建连高峰 worker 0 触发 2 次 longSched 告警（>50ms 单协程霸占，
   sysmon 观测口径），无卡死、无误迁、零业务错误。
3. somaxconn=128 是同机万级突发的潜在重置源；本次 BATCH=100 错峰恰好
   不溢出，更高突发速率（或 Linux 生产部署）应调大 somaxconn 或保持错峰。

**结论：10000 长连容量基准在 2 核 8GB 笔记本、未提权改系统参数的条件下
一次达标——10000/10000 建连、保活收发零错误、worker 分布偏差 0%、
65.7KB/连接、fd 完全守恒。** 后续性能输入：GC 协程注册表锁分片
（吞吐优化，非容量缺陷）。
