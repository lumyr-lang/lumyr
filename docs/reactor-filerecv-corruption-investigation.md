# file.recv 类型串改问题 —— 根因结案报告

> 日期：2026-10-01 起，2026-10 结案。
> §1–§9 为结案前的阶段性取证记录（保留原貌，H2 当时仅为待裁决假设）；
> **§10 为最终结案：H2 经 A/B 实验证实为唯一根因，已正式修复并全量验证。**
> 所有改动均未提交（无 commit / 无 push）。

---

## 0. 一句话结论

> **【结案更新】** 根因即旧报告 §5 的 **H2**：`LM_BUMP_REDS` 时间片抢占发生在 VM
> 操作数栈**非平衡**派发点（CALL/BUILTIN/CALL_METHODV/JMP，实参含 receiver 正压栈），
> reds 耗尽即 slice_yield；协程 yield/resume 只存/恢复 per-thread 共享栈的 `sp` 下标、
> 不搬数据区，让出协程被 FIFO 重入同线程队尾后，其栈上活 Value 被后运行协程覆盖
> （他人的 FileObj 串进 conn/receiver）。修复：抢占前经 `can_preempt` 安全门校验
> "4 栈 sp == 本轮 resume 入口栈深"，非平衡点只借时间片、到下一平衡点再让。
> 详见 §10。

- 现象不是 GC 回收、不是堆内存越界/UAF；报错位置上的 `file` 是一个 **GC 头完全合法的 FileObj**。
- 已查明之前两轮取证的**两个自身漏洞**（GC 归属漏埋 TLA 快路径；帧 dump 只看 `cnt` 没看 `cap`），
  因此"`<OTHER-CO>` 跨协程污染"的旧判定**不成立、作废**。
- 当前证据把问题收敛到一个很窄的面：**某协程栈帧的参数槽 `conn`（vals[1]）在请求处理中途
  被写成了本协程或其他协程的局部值 `f`（file）**。下一轮用 4 个定点金丝雀即可在一次复现内
  区分"写错帧 / 栈不平衡错位 / h.conn 字段被改 / 协程栈被跨线程写"四种机制。

---

## 1. 现象与复现

- 服务端：`tests/probe/stream_reactor_server.lm`（新增、未跟踪）。
  启动：`PORT=19210 WORKERS=4 MAX_CONN=128 BACKLOG=1024 CONN_CAPACITY=4096 bin/lumyr ...`
- 客户端：50 个线程对同一端口突发上传 10MB（`R:<len>\n` → READY → body → `DONE\n` → OK）。
- 默认 -O2 构建：几乎每轮恰好 **1 个**连接失败（49p/1other），worker 随机，耗时约 30~42s；
  插桩加重后曾出现 17p/33other，竞态对时序高度敏感。
- 失败形态：客户端已发完全部 10MB、连接早已回 READY，服务端在
  `done = readLine(conn)`（等 DONE）这一步抛 `类型'file'不支持方法'recv'`。
- 即：`conn` 在首行 `readLine`、整个 `recvToFile` 期间都是好 socket，
  **经过 recvToFile（内部 f.appendBytes 高频走 blocking 池跨线程）之后变成了 file**。

## 2. 已排除的假设（附证据）

| # | 假设 | 排除证据 |
|---|------|----------|
| E1 | GC 把 socket 回收后内存复用为 file | `LUMYR_GC_STATS=1` 下 50×10MB 复现时 GC 日志 0 行：Minor/Major 从未运行，无 sweep；关闭增量 STW 仍复现。socket/file 均 `gc_alloc` 无显式 free。 |
| E2 | 堆越界写 / UAF / 野指针 | clang `-O1 -fsanitize=address` 构建下 20×10MB、50×10MB 全部通过且 **0 ASAN 报告**；错误只在 -O2 高并发时序出现。 |
| E3 | conn 槽的类型标签被踩坏、裸指针乱指 | 报错点 dump：`Value.type=24`(VAL_FILE)，GC 头 `vtype=24 user_size=128 marked=3 age=0`，与 FileObj 完全吻合——槽里装的是**一个真正的 file 对象本身**，不是垃圾值。 |
| E4 | VALUE 操作数栈压/弹错位导致 receiver 取错 | 在 `vm_exec_call_method_dyn()` 弹 receiver 处 dump：报错时 `VALUE sp=0`（调用边界栈平衡）。 |
| E5 | 协程跨 worker 迁移导致 fd/帧串台 | SO_REUSEPORT 下内核按五元组 hash 固定 worker；reactor/fd_map/TLS 栈池均 per-thread。跨线程迁移另有 mig_copy 搬栈机制（见 §4）。 |

> 注：E4 只证明"调用 builtin 的那个瞬间 VALUE 栈平衡"，不排除**更早的某次 yield 时栈不平衡、
> 活值被同线程其他协程覆盖后又被 STORE 进帧槽**（见 H2）。

## 3. 上一轮"跨协程污染"判定为何作废（两个取证漏洞）

1. **GC 归属表漏埋第三个分配返回点。**
   `gc_alloc()`（`kit/runtime/src/gc_runtime.c`）对小对象有三条返回路径：
   - TLA 本地空闲链表命中 → **L452 `return obj_to_ptr(cur)`（漏埋）**；
   - 本地链表未命中、batch malloc 16 个 → L519（已埋）；
   - 大对象直接 malloc → L554（已埋）。
   FileObj(user_size=128) 在多数 worker 上直接复用进程启动期其他同尺寸对象留下的
   per-thread 预分配块，全部走 L452，所以归属表 `file_allocs=0`、坏 file `hit=0`。
   **hit=0 不能推出 OTHER-CO。**
2. **帧 dump 只遍历了 `[0, cnt)`，局部变量不可见。**
   参数（self/conn）经 `stackframe_bind` 带名字入帧（cnt=2）；而函数局部变量
   （id/header/f/...）由 `OPC_STORE_VAR` 按编译期槽号 **直写 `frame->vals[idx]`，
   不增加 cnt、不挂 names**（见 `src/ir/vm_exec_var.c` L198-211）。
   所以 dump 里 lvl1 帧"只有 self+conn"是假象，**f 实际就在该帧 vals[2..cap) 的某个槽里，
   上一轮根本没打印出来**。

## 4. 相关运行机制（本轮读码确证）

1. **变量存储**：参数与局部变量统一存当前 `StackFrame` 的平行数组
   （vals / int_slots / flt_slots / ptr_slots / type_tags / refs），槽位由编译器编号；
   VALUE 等四类操作数栈只用于表达式求值的临时中转。
2. **协程切换只存下标，不存栈数据**（`src/ir/vm_co.c` L67-141）：
   - yield：保存 4 栈的 `sp` 下标、err_jmp、异常状态到 `VMCoState`，随后把 TLS 的 sp
     恢复为调用方基线；**不保存 VALUE 等栈的数据区**；
   - resume：仅把 sp 下标写回 TLS；
   - 仅当发生跨线程迁移（`migrate_sched`）时才把 `stacks[i][0..sp)` memcpy 到 mig_copy，
     到目标线程写回。
   - 推论：正确性依赖"**yield 点操作数栈平衡、所有活值都在堆上的帧槽里**"。
3. **帧链现场（报错时，12 层遍历实际 6 层）**：

   | 层 | 帧内容 | 说明 |
   |----|--------|------|
   | lvl0 | `readLine`：仅 `conn`(=file) | 自由函数帧，cnt=1 |
   | lvl1 | `self`(VAL_PTR) + `conn`(file) | 疑似 UploadHandler.serve |
   | lvl2 | `self`(VAL_PTR) + `conn`(file) | 疑似 ServiceApplication.handleConnection |
   | lvl3 | `self`(VAL_PTR) + `conn`(file) | 疑似 ServiceWorker.handleConnection |
   | lvl4 | `h`(VAL_CLASS_PTR, ServiceConnectionHandler) | 协程入口闭包帧 |
   | lvl5 | 空帧，parent=NULL | — |

   三层 `conn` 槽是**同一个 file 指针**，说明错误值沿委托链一路透传，源头在链顶端
   （协程入口 `c = h.conn`）或在帧槽被定点改写。
   待解释：三层方法接收者 `self` 类型是 VAL_PTR(120)，而 lvl4 的 `h` 是 VAL_CLASS_PTR(13)；
   需确认方法绑定路径是否把 class 实例拆箱成 VAL_PTR 传递（不影响主结论，列为旁证）。
4. **唯一跨线程高频操作**：`f.appendBytes(buf)` → `lumyr_file_append_bytes()`
   → `lm_co_await_blocking(chunk_append_blocking, &c, NULL)`。其 `chunkWriteCtx c`
   是**协程 C 栈上的局部结构体**，指针交给 blocking 池工作线程；协程随即 yield，
   期间存在协程栈 swap-out（stealable 机制）的交互窗口。
5. **另一个被忽略的 yield 触发点**：日志中出现过 `handler budget overrun forceYield`
   （预算耗尽强制让出）。它可能在任意字节码边界触发，与 fd_wait/blocking 并列，
   需要单独核对其让出时的栈平衡前提。
6. **GCObject 头**：16 字节，新对象 `marked = g_gc_marking ? 1 : 3`，
   故坏 file 的 `marked=3` 是正常初值，无异常。

## 5. 当前待裁决假设（按可能性排序）

- **H1 帧槽定点写错（最符合"值恰好是 f、稀有、不崩溃"的统计特征）**
  - H1a：某次 `STORE_VAR` 的 `ctx->frame` 指向了错误帧（调用/异常/恢复路径上帧指针串台），
    把 file 写进了另一协程或另一调用的 vals[1]；
  - H1b：编译器槽位 fixup/复用错误，某条 STORE 的 idx 落到参数槽 1；
  - H1c：refs 别名（`RefDesc.ptr`）把对某变量的写转发到了 conn 槽。
  - 判别：完整 dump serve 帧 `vals[0..cap)` + fn 名 + 槽号↔变量名。
    若 f 自己的槽也是同一个 file → 帧/槽级错位；若仅 vals[1] 异常 → 定点写 idx=1。
- **H2 yield 点 VALUE 栈不平衡**：appendBytes/co_wait_fd/forceYield 中某条让出路径
  在 sp 高于基线时让出，同线程其他协程复用该栈区，恢复后调用边界取到他人的 file 再入槽。
  判别：yield_hook 加"sp == 本协程 resume 基线"金丝雀断言。
- **H3 `h.conn` 字段在构造后被改写**：若协程入口读出的 `h.conn` 已是 file，
  则问题在 class 实例字段存储 / 构造器帧 / `STORE_FIELD`，不在 serve。
  判别：onAccepted(createHandler) 与协程入口两处记录 h 地址、conn 的 (type,ptr,fd) 对照。
- **H4 blocking 线程写协程栈 + 栈 swap 竞态**：chunkWriteCtx 留协程栈，
  forceYield/迁移路径在 blocking 完成前换栈（正常 stealable=0 应阻止 DONTNEED，需核对例外）。
- **H5 frame freelist 跨协程复用 / 帧提前 destroy**：值精确且 ASAN 无报告，可能性最低。

## 6. 下一轮取证计划（4 个金丝雀，一次复现裁决）

> 原则：只加观测、不改行为；在"值的源头/每次跨越边界"打点，而不是在错误终点倒推。

1. **修正帧 dump**：打印 `vals/int_slots/ptr_slots/type_tags/refs` 的 `[0, cap)` 全槽、
   BytecodeFunc 全限定名、槽号↔变量名映射；确认 lvl1 即 serve、f 的槽号、
   conn 槽 vals[1] 与 f 槽的关系。
2. **file 归属补点**：在 `lumyr_file_make()`/`lumyr_file_from_bytes()` 出口直接记录
   `(ptr, lm_co_current(), pthread_self())`（绕开 gc_alloc 三返回点问题），
   判定坏 file 由哪个协程/线程创建。
3. **yield 平衡断言（H2 裁决）**：`vm_co_yield_hook` 中对 4 栈比较
   `sp` 与该协程记录的 resume 基线（非迁移场景），不平衡即打印协程身份/差值/当前 fn 并中止。
4. **conn 链路打点（H3/H1 裁决）**：accept 返回处 → createHandler(h) →
   协程入口 `c=h.conn` → handleConnection 入口 → serve 入口，逐跳记录
   `co / h / fn / (type,ptr,fd)`；定位 file 第一次出现的那一跳。

判读表：
- 金丝雀 3 先触发 → H2（栈平衡被破坏），按具体内建路径修；
- 金丝雀 4 中 `h.conn` 在协程入口前已变 → H3，查 class 字段/构造器；
- 金丝雀 4 各跳正常、但 serve 内某次 appendBytes 后 vals[1] 变 file → H1/H4，
  用金丝雀 1/2 的帧全槽 + file 创建协程定位写错者。

## 7. 工作区改动盘点（git，均未提交）

### 7.1 正式功能 / 修复（保留）

| 文件 | 内容 |
|------|------|
| `kit/runtime/src/lm_thread.c`、`include/lm_thread.h` | 新增 `thread_detach(id)`：分离线程退出即回收线程表槽位，含 done/detached 竞态论证与 EINVAL 处理（双语错误）。 |
| `src/ir/bytecode_type.h`、`src/ir/ir_compile.c` | 注册 `BUILTIN_THREAD_DETACH` / `thread_detach` 内建名。 |
| `kit/runtime/src/lm_co.c`、`include/lm_co.h` | coSleep timer 所有权 UAF 修复：回调不 free ctx，仅 wakeup + 原子置 sleep_arm=FIRED（ARM/FIRED 协议），ctx 由协程侧单一所有者释放。 |
| `kit/runtime/src/lm_socket.c` | `socket_sys_somaxconn()`（Linux/macOS），listen backlog 被内核静默截断时打双语 [warn]，不改变行为。 |
| `lumyr-lms/LumyrApp/ServiceApplication.lm` | acceptQueue 排队模式（+86 行）：在役达 maxConnections 时暂停 accept 让连接留在内核 backlog 排队，含 setAcceptQueue/acceptQueueEnabled/acceptGateMs/reapDueHandlers。 |
| `tests/probe/stream_reactor_server.lm` | 新增：reactor 上传复现/验收服务（未跟踪文件）。 |

### 7.2 临时诊断插桩（取证结束必须删除）

| 文件 | 内容 |
|------|------|
| `kit/runtime/src/gc_runtime.c`、`include/gc_runtime.h` | LM_DUMP_CONN file 归属环形表（4096）、`diag_note_file`、`lumyr_gc_diag_file_owner/count`；gc_alloc 两个返回点埋点（**L452 快路径漏埋**）；include "lm_co.h"。 |
| `src/ir/vm_builtin.c` | include `<pthread.h>`；`bi_type_err()` 内 DUMP-CONN（Value/GC 头/file owner）。 |
| `src/ir/vm_exec_call.c` | include `<pthread.h>`；`vm_exec_call_method_dyn()` 内 DUMP-RECV + 12 层帧遍历（**只到 cnt，看不到局部变量**）。 |

回滚边界：删除 7.2 三处插桩即可还原 7.1 的干净状态；7.1 各文件互不影响删除动作。

### 7.3 构建 / 进程状态

- `bin/lumyr` 当前为带全部插桩的 -O2 构建；`make all` 正常
  （IDE/clangd 的头文件找不到等告警是 include 路径噪声，可忽略）。
- `/tmp/lumyr_normal_backup` 是**更早**的干净二进制，落后于当前 7.1 正式改动，不可直接覆盖使用；
  恢复干净版需删除插桩后重新 `make all`。
- ASAN 构建命令与复现命令见 §8。取证服务端进程已全部停止。

## 8. 复现 / 构建命令备忘

- 正常构建：`find src kit/runtime -name '*.o' -delete && make all`
- ASAN 构建：
  `make all CC=clang CFLAGS="-Wall -Wextra -g -O1 -I./src -I./build/gen -I./kit/runtime/include -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address"`
  运行：`ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0"`
- 重测前：`pgrep -fl "bin/lumyr"` 无残留；`lsof -ti:19210` 唯一；`rm -f /tmp/lumin_probe_upload_*.bin`
- 协程内禁 sleep，等待用 coSleep；改 C 必须 make，改 lumyr-lms/*.lm 运行即生效。

## 9. 验收标准（修复后）

1. 多 worker（4）50×10MB 连续多轮 **50/50，零 file.recv**；
2. 200 连接错峰全过、零 RST（或 RST 仅来自 somaxconn 上限且有告警）；
3. ASAN 构建同样零报告；RSS 随规模稳定不漂移；
4. 既有回归：branch_retype / file / socket / web_* / conn_capacity(workers=2) /
   stress_concurrent 4000 长连 / service_co；
5. 删除 §7.2 全部插桩，重新 make 干净构建后复测 1；
6. 更新 `.trae/skills/lumin-stream-file-transfer/SKILL.md`
   （reactor+workers、acceptQueue、thread_detach、coSleep ARM/FIRED、somaxconn；
   教训：多 worker 偶发类型串改先证伪 GC，再查跨线程共享状态/协程栈/帧槽；
   取证注意 gc_alloc 三条返回路径与帧槽 idx 直写两个坑）。

---

## 10. 结案：H2 证实、正式修复与全量验证

### 10.1 四个金丝雀的裁决结果（对应旧 §6）

- 金丝雀 3（yield 平衡断言）率先触发：报错前捕获到 **10 次"非平衡让出"**
  （VALUE 等栈 `sp` 高于该协程本轮 resume 入口栈深），时序为
  `#1 非平衡让出 → recv 类型错误 → #2 丢值(delta=-1)`，与报错一一咬合。
- 金丝雀 1/2/4 排除 H1/H3：坏 FileObj 是其他协程在**共享操作数栈数据区**压入的活值，
  并非帧槽定点写错、也非 h.conn 构造后被改。
- 机制坐实：`LM_BUMP_REDS`（`src/ir/vm_exec.c` 的 CALL/BUILTIN/CALL_METHODV/JMP/MKCLOSURE/
  CALLV 派发点）在 reds 耗尽时置 slice_yield 并立即 `lm_co_yield`；此刻实参（含 receiver）
  正压在 4 个 per-thread 共享栈（`g_stack_mgr->stacks[]`，`_Thread_local`）上。
  yield/resume hook（`src/ir/vm_co.c`）只保存/恢复 **sp 下标**，数据区不搬；
  slice 协程在 `lm_co.c` 被 `lm_scheduler_post` **FIFO 重入同线程队尾**，
  同线程后运行协程在同一数据区压值，覆盖前协程活值，resume 后弹出的实参/receiver 即被串改。

### 10.2 A/B 因果实验（同一二进制，env 门控）

| 组 | 变量 | 结果 |
|----|------|------|
| B：`LM_DISABLE_SLICE_YIELD=1`（禁用 slice 抢占） | 无任何非平衡让出 | 50×10MB × 10 轮 **500/500 成功**，非平衡让出 0 |
| A：恢复 slice 抢占 | 10 次非平衡让出 | 249/250，**1 次 recv 类型错误**，时序与非平衡让出咬合 |

唯一变量 = 非平衡 slice 抢占，因果闭环。（实验门控代码取证后已删除。）

### 10.3 正式修复（方案 A：抢占安全门 hook，最小正确）

- `kit/runtime/include/lm_co.h`
  - 声明 `void lm_co_slice_bump(lm_co_t* co)`、`typedef int (*lm_co_can_preempt_hook_t)(lm_co_t*)`、
    `lm_co_set_can_preempt_hook()`。
  - `LM_BUMP_REDS` 宏改为：reds 归零后调 `lm_co_slice_bump(_co)`
    （原"内联置 slice_yield=1 + lm_co_yield"移除）。
- `kit/runtime/src/lm_co.c`
  - `static g_co_can_preempt_hook` + 注册函数。
  - `lm_co_slice_bump`：hook 存在且判定**非平衡** → `co->reds=LM_SCHED_REDS`
    （借一个时间片，到下一平衡派发点——JMP 回边 / callee 弹参后派发点——再让）；
    平衡则置 slice_yield=1 并 `lm_co_yield`。无 hook（纯 C 宿主）直接让出，行为不变。
- `src/ir/vm_co.c`
  - `VMCoState.entry_sp[4]`：resume_hook 记录本轮入口 4 栈栈深（**修复依赖，非插桩**）。
  - `vm_co_can_preempt()`：无 vm_state/无 g_stack_mgr 放行；否则要求 4 栈 sp 全等于 entry_sp。
  - `vm_co_hooks_register()` 注册该 hook。
- 边界说明：跨线程迁移 yield（blocking 池，`migrate_sched`）本就在平衡态（实参已弹）且
  另有 mig_copy 搬数据，不经安全门；IO 等待让出（co_wait_fd 等）也是弹参后的平衡态。
  cc 通道（ir_cgen 当前已清空待重构）未来落地时需保证活值先溢出到栈帧再 yield。

### 10.4 金丝雀清理

7 处临时插桩全部删除并干净重建（全仓 grep
`diag_dump_frame|diag_note_file|UNBALANCED|dbg_fn_name|LM_DUMP_CONN|LM_PREEMPT_ALWAYS|DUMP-CONN|lumyr_gc_diag|DBG-MISMATCH`
零命中）；`entry_sp[4]` 作为正式修复保留。工作区仅剩 §7.1 正式改动 + 本修复三处文件。

### 10.5 修复后验证（全部通过）

- **主战场（正常构建）**：50×10MB 连跑：带插桩修复版 750/750、**清理插桩后干净版
  300/300 + 终验 100/100**；200 错峰 200/200；服务端 recv 类型错误 0、handler 异常 0。
- **ASAN 构建**：主战场 50×10MB × 4 轮 **200/200，零 ASAN 报告、零类型错误**。
- **回归（VM 通道）**：branch_retype / file / socket / web_error / web_filter / web_limits /
  service_co / conn_capacity(workers=2) 全 PASS；stress_concurrent **4000 长连 PASS**；
  VM 全量（排除脚本既有 skip 集）**85 pass / 0 fail**。
- 验收标准旧 §9 全部满足。

### 10.6 附带发现并已修复：B29 触发的 ASAN bigint 脏指针段错误（定长字段 null 写入缺陷）

- 现象：http_param_type_test 仅在 **ASAN 构建**下稳定段错误，栈为
  `lumyr_bigint_to_string`（lm_bigint.c）← `value_to_str` ← `OPC_CAST_STRING`，
  Value 类型标签是 VAL_BIGINT、载荷却是 Mach-O 主二进制基址 `0x100000000`
  （非 ASAN 下栈残留恰为 0，测试靠运气通过）。
- 曾用"reds 调大 1000 倍仍必崩"排除与 slice 抢占相关，方向正确；但初判
  "嵌套字段越界 → bigint 脏、独立既有缺陷"**定位不准，已由本次精确定位推翻**：
  崩溃请求其实是 **B29**（`{"company":"ACME","founded":"xyz"}`，lldb 验尸
  CompanyIn 实例 company="ACME"、dept=NULL、founded 槽=0x100000000），
  与深嵌套/越界无关。
- 真正根因（定长对象字段写入的 null 语义缺口，两处叠加）：
  1. 生成器 `src/parse/boot_gen.c` 的 `__lm_model_<Class>` 对声明类型字段发射
     `if(m.contains(f)) inst.f = RequestBinder.asXxx(m[f])`，binder 拒绝（非法/越界）
     按设计返回 null（注释明示"直接赋 null，不报错"）；
  2. `val_none()`（lumyr_value.c）构造 VAL_NONE 时**不清空联合体**，载荷为不确定栈残留；
  3. `lumyr_field_set_trusted()`（lm_type.c）对 PTR 族字段只有 STRING 有
     "VAL_NONE → NULL" 分支，bigint/decimal/bitdecimal/bytes/容器/类实例等一律
     裸拷 `value.v.struct_ptr`，把 none 的垃圾载荷落进定长字段；
  4. handler `c.founded != null` 读垃圾为真，`(string)c.founded` 按 BigInt* 解引用崩溃。
- 修复：
  - lm_type.c：PTR 族字段写入遇 VAL_NONE 一律置 NULL（与 STRING 分支、读取侧
    "空指针读回 none"语义对齐）；
  - lumyr_value.c：`val_none()` memset 清零联合体（防御同类隐患）。
- 验证：ASAN 下 http_param_type 10/10 ALL PASS、零 ASAN 报告；正常构建 10/10 +
  VM 全量 85/0；主战场 file.recv 100/100 无交互回归。
- 另发现并已修复一个原以为"预存在、与本任务无关"的 ASAN 问题：arrow_func_test.lm
  在 ASAN 下报 `stack-use-after-scope`（vm_exec_arith_ptr_add 读到死栈串）。
  真因见 §10.8，修复后 ASAN 全量 85/0。

### 10.7 最终工作区（均未提交）

正式修复：
- slice 抢占根因：`lm_co.h` / `lm_co.c` / `vm_co.c`（can_preempt 安全门 + entry_sp）；
- bigint 脏指针根因（§10.6）：`lm_type.c`（PTR 族字段 null 落 NULL）、
  `lumyr_value.c`（val_none 清零联合体）；
- SSO 默认串悬空根因（§10.8）：`vm_exec_call.c`（默认参数 PTR 分支 SSO 物化 GC 串）。
其余正式功能同旧 §7.1：thread_detach、coSleep ARM/FIRED UAF、somaxconn 告警、
ServiceApplication acceptQueue、thread_detach 字节码注册、stream_reactor_server.lm。
当前 `bin/lumyr` 为无插桩正常构建；ASAN 构建产物已由正常构建覆盖。

### 10.8 修复：arrow_func_test 的 ASAN stack-use-after-scope（动态调用 SSO 默认串悬空）

- 现象：仅 ASAN 构建下 arrow_func_test 报 2 次 `stack-use-after-scope`
  （strlen 读 6 字节、memcpy 读 5 字节 = 串 `"hello"`），逻辑 ALL PASS，正常构建
  读栈残留侥幸正确——典型潜伏 UB。触发点是 PART4 默认参数
  `greet_name = (name: string, greeting: string = "hello"): string => ...`。
- 因果链：
  1. 顶层箭头函数变量调用编译为 OPC_CALLV → `vm_call_func_value`；
  2. 实参不足时 `eval_default_expr`/`eval_literal_default` 在默认值填补循环的
     **C 栈块局部 `Value dv`** 中现场求值，AST_STRING 经 `lumyr_make_string` 对
     len≤SSO 上限的短串产出**内联 SSO Value**（字节就在结构体内）；
  3. EXPR_TYPE_PTR 绑定分支（原 vm_exec_call.c:972-979）直接取
     `dv.v.sso.data` 裸指针 `stackframe_bind_ptr` 进新帧 ptr_slots；
  4. 本轮块作用域结束（ASAN -O1 毒化该栈槽），callee 体内 `greeting + ", "` 经
     PTR 栈拼接 strlen/memcpy 读到已毒化栈 → 报告。
- 对照实验（修复前最小用例）：SSO 默认串必现 2 报告；长默认串（>SSO 走
  gc_alloc 堆）与无类型形参（走 stackframe_bind 整值拷贝）零报告——三条路径
  恰好区分出唯一缺口在"临时量 + 裸指针"组合。
- 修复（vm_exec_call.c 默认参数 PTR 分支）：SSO 默认串先 `gc_alloc+memcpy`
  物化为 GC 串再绑定；非内联串 v.s 本就是 gc_alloc 堆内存、生命周期独立于 dv，
  直接绑定。正常实参路径不拷贝的契约（源 Value 在整个调用期存活）对"现场求值
  临时量"不成立，故修绑定点而非拼接函数。
- 验证：最小三对照用例转净；arrow_func ASAN 5/5、正常 10/10；ASAN 全量 VM 85/0
  （原唯一 ASAN 失败清零）、正常全量 85/0；20 万次默认串调用 GC 压力 PASS
  （ptr_slots 是 GC 根，新串不会被提前回收）。
