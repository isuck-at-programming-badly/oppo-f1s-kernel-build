/*
 * Stub definitions for scheduler debug functions when CONFIG_SCHED_DEBUG is disabled.
 * These are called from fair.c and stop_task.c.
 */
#include <linux/sched.h>

void TaskTh(unsigned int B_th, unsigned int L_th) {}
void HmpStat(void *hmp_stats) {}
void HmpLoad(int big_load_avg, int little_load_avg) {}
void RqLen(int cpu, int length) {}
void CfsLen(int cpu, int length) {}
