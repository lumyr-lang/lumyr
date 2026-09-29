// lm_sysmon.h —— Phase 8.5 D：sysmon 守护线程
// 对标 Go runtime sysmon（proc.go:6537）：带外扫描所有 scheduler，
// 检测卡在长协程上的 scheduler（schedtick 超时未动），触发强制迁移。
//
// 分工：
//   - drain 内 C 层长调度告警（>50ms，子阶段 C）：抓 C 内建忘 BUMP_REDS。
//   - sysmon 跨 scheduler 扫描（>10ms）：抓死循环协程，写 migrate_sched
//     迁到 compute 池（子阶段 E）。
//
// sysmon 懒启动：首次 lm_scheduler_new 时启动单例线程；进程退出时 atexit 停止。
#ifndef LM_SYSMON_H
#define LM_SYSMON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 sysmon 守护线程（幂等：多次调用只启动一次）。
 * 由 lm_scheduler_new 首次调用时触发，应用代码无需显式调。 */
void lm_sysmon_start(void);

/* 停止 sysmon 守护线程（atexit 自动调，也可显式调）。 */
void lm_sysmon_stop(void);

/* sysmon 当前是否已启动（测试/调试用）。 */
int lm_sysmon_running(void);

#ifdef __cplusplus
}
#endif

#endif /* LM_SYSMON_H */
