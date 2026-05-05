/* sys_stat — periodic system observability sampler for TAISHAN PAI.
 *
 * Every SAMPLE_SEC seconds, snapshot a batch of kernel-exposed metrics and
 * append a human-readable, multi-line block to /blackbox/sys_stat/stat.<N>.
 * Slot rotation is per-boot (1..MAX_SLOTS, wraps around; current slot
 * number persisted in /blackbox/sys_stat/index_now).
 *
 * =====================================================================
 * Output format (one block per sample)
 * =====================================================================
 *
 *   [2026-04-23 14:25:50] uptime=0d:00:05:12
 *     LOAD : load1=0.45  load5=0.22
 *     CPU  : user=3.2%  sys=1.1%  iowait=0.5%  idle=95.2%
 *     FREQ : cpu0=1800  cpu1=1800  cpu2=1800  cpu3=600  (MHz)
 *     TEMP : soc=45.2C  gpu=43.8C
 *     MEM  : total=1985432KB  avail=1234567KB  cached=687654KB  used=37.8%
 *     NET  : wlan0 rx=2.3/tx=15.7  |  wlan1 rx=125.4/tx=8.9  (KB/s)
 *     TOP5 : myapp[1234]=45.2%  hostapd[567]=12.3%  kworker[89]=5.1%  ...
 *
 * =====================================================================
 * Metric reference (what each field means, where we got it from)
 * =====================================================================
 *
 *   TIMESTAMP (first line)
 *     本地时间 (依赖 /etc/localtime, 由 time_sync 线程/buildroot 配置好).
 *     来源: time(NULL) + localtime_r() + strftime.
 *
 *   uptime
 *     自内核启动到现在经过的时间, 与系统时钟(是否 NTP 同步)无关.
 *     即使系统时间仍停在 2017, uptime 依然准确, 排查"开机后多久出事"很有用.
 *     来源: /proc/uptime 第一个字段(秒, 浮点).
 *
 *   LOAD
 *     load1 / load5 = 最近 1 / 5 分钟的平均 runnable+uninterruptible 进程数.
 *     注意: 不是 CPU 占用率. RK3566 是 4 核, load1≈4.0 代表 4 核跑满;
 *     load1=8.0 代表还有 4 个进程在 runqueue 里排队. load 被 IO 阻塞的
 *     进程也算进去, 所以"load 高 CPU 不忙"的场景 (eMMC 卡住/IO 死锁)
 *     光看 load 看不出来, 要配合下面的 iowait%.
 *     来源: /proc/loadavg (第一、二个字段).
 *
 *   CPU
 *     user%   = 用户态 CPU 时间占比 (不含 nice 调高优先级的进程)
 *     sys%    = 内核态 CPU 时间占比 (包含 system + hardirq + softirq)
 *     iowait% = CPU 因等待磁盘 IO 而空闲的占比
 *                ^ 如果这个持续高, 说明瓶颈在存储 (eMMC/SD)
 *     idle%   = CPU 完全空闲占比
 *
 *     计算方法: 两次采样之间, /proc/stat 第一行 "cpu  X Y Z ..." 各字段的
 *     增量, 除以总增量得到百分比. 这是跟 `top` / `mpstat` 同一套指标.
 *     来源: /proc/stat 首行 (差分).
 *
 *   FREQ
 *     每个 CPU 核当前的运行频率 (MHz). RK3566 4 个 A55 核, 正常跑 ~1800MHz,
 *     空闲降到 ~408/600MHz 省电. 如果该跑满的时候频率没上来, 是 DVFS
 *     调速器配置或 thermal 压频. 结合 TEMP 判断.
 *     来源: /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq (KHz, 转 MHz).
 *
 *   TEMP
 *     SoC 内各温区当前温度 (°C). zone 名字从 .../type 文件读. RK3566 ~85°C
 *     会触发 thermal throttle 降频; 持续 90°C+ 接近保护限.
 *     来源: /sys/class/thermal/thermal_zoneN/temp (milli-°C, N = 0,1,2...).
 *
 *   MEM
 *     total   = 物理内存总量 (去除 kernel reserve 后).
 *     avail   = kernel 估算的"立即可用内存", 包含可回收的 page cache.
 *                ^ 这才是"还剩多少"的真实答案, 不要看 free
 *     cached  = 文件页缓存 (Linux 会用空闲内存缓存读过的文件, 属正常)
 *     used%   = 100 * (total - avail) / total  (用 avail 而非 free 算, 诚实)
 *     来源: /proc/meminfo (MemTotal / MemAvailable / Cached).
 *
 *   NET
 *     每个网口最近 SAMPLE_SEC 秒的平均 RX/TX 吞吐 (KB/s). 只显示有流量的口,
 *     回环 lo 和 dummy 不显示. 做被动流量测试 (手机发给板子多少字节/秒)
 *     这个就够用, 不用 iperf3.
 *     来源: /proc/net/dev 每行的 rx_bytes / tx_bytes (差分 / 时间).
 *
 *   TOP5
 *     最近 SAMPLE_SEC 秒内 CPU 时间涨得最多的 5 个进程, 跟 `top` 的 %CPU
 *     列语义一致: 一个进程吃满一个核 = 100%. RK3566 4 核, 所以 4 个进程
 *     各占一核 = 4x100% = 400%.
 *     计算: 对 /proc/<pid>/stat 的 utime+stime (CPU ticks), 两次采样做差,
 *     除以 (SAMPLE_SEC * sysconf(_SC_CLK_TCK)) 得百分比.
 *     格式: COMM[PID]=XX.X%  — COMM 是 /proc/<pid>/stat 第二字段(15 字符内).
 *     来源: 遍历 /proc/<数字>/stat 文件.
 *
 * =====================================================================
 * Storage / rotation
 * =====================================================================
 *
 *   LOG_DIR    = /blackbox/sys_stat
 *   slot file  = stat.1 .. stat.MAX_SLOTS  (每次进程启动换下一个)
 *   SIZE_CAP   = 10 MiB — 单个 slot 文件写满就停, 避免吃爆 /blackbox 分区
 *   SAMPLE_SEC = 5 秒 — 平衡粒度与开销
 *
 *   注意: /blackbox 掉电易丢, 而且我们板子的 RTC 冷重启也掉, 所以开机初
 *   几秒时间戳会是 2017; uptime 始终准. 后处理时建议以 uptime 对齐.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define LOG_TAG "sys_stat"
#include "log.h"

#include "sys_stat.h"

#define LOG_DIR     "/blackbox/sys_stat"
#define INDEX_FILE  LOG_DIR "/index_now"
#define MAX_SLOTS   20
#define SAMPLE_SEC  5
#define SIZE_CAP    (10UL * 1024UL * 1024UL)

#define NUM_CPUS        4          /* RK3566 */
#define MAX_THERMAL     8          /* 尝试 zone0..zone7 */
#define MAX_IFACES      8          /* 最多跟踪 8 个网口, 足够 eth0/wlan0/wlan1 */
#define MAX_PROCS       512        /* 一般板子同时跑的进程数 */
#define TOP_N           5          /* TOP5 */
#define COMM_LEN        16         /* Linux task comm 最长 15 + NUL */

/* ================================================================== */
/* 持久化状态: slot 索引                                                */
/* ================================================================== */

static int read_index(void)
{
	FILE *f = fopen(INDEX_FILE, "r");
	int idx = 0;
	if (f) {
		if (fscanf(f, "%d", &idx) != 1)
			idx = 0;
		fclose(f);
	}
	if (idx < 0 || idx > MAX_SLOTS)
		idx = 0;
	return idx;
}

static void write_index(int idx)
{
	/* 原子替换: 写到 .tmp 再 rename, 避免断电时 index 文件被截断. */
	FILE *f = fopen(INDEX_FILE ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "%d\n", idx);
	fclose(f);
	rename(INDEX_FILE ".tmp", INDEX_FILE);
}

/* ================================================================== */
/* 小工具: 读文件第一行 / 第一个数                                      */
/* ================================================================== */

static void read_first_line(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;

	buf[0] = '\0';
	if (!f)
		return;
	if (fgets(buf, (int)cap, f) == NULL)
		buf[0] = '\0';
	fclose(f);
	n = strlen(buf);
	if (n && buf[n - 1] == '\n')
		buf[n - 1] = '\0';
}

static int read_long_from(const char *path, long *out)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int ok = (fscanf(f, "%ld", out) == 1);
	fclose(f);
	return ok ? 0 : -1;
}

/* ================================================================== */
/* 1) 内存 — /proc/meminfo                                              */
/* ================================================================== */

struct meminfo {
	unsigned long total_kb;
	unsigned long free_kb;
	unsigned long avail_kb;
	unsigned long buffers_kb;
	unsigned long cached_kb;
};

static void read_meminfo(struct meminfo *m)
{
	char line[256];
	FILE *f;

	memset(m, 0, sizeof(*m));
	f = fopen("/proc/meminfo", "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if      (sscanf(line, "MemTotal: %lu kB",     &m->total_kb)   == 1) continue;
		else if (sscanf(line, "MemFree: %lu kB",      &m->free_kb)    == 1) continue;
		else if (sscanf(line, "MemAvailable: %lu kB", &m->avail_kb)   == 1) continue;
		else if (sscanf(line, "Buffers: %lu kB",      &m->buffers_kb) == 1) continue;
		else if (sscanf(line, "Cached: %lu kB",       &m->cached_kb)  == 1) continue;
	}
	fclose(f);
}

/* ================================================================== */
/* 2) CPU 分解 — /proc/stat 首行差分                                    */
/* ================================================================== */

struct cpu_stat {
	/* 对应 /proc/stat 首行的字段 (单位: ticks = 1/CLK_TCK 秒) */
	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
};

static int read_cpu_stat(struct cpu_stat *c)
{
	FILE *f = fopen("/proc/stat", "r");
	if (!f)
		return -1;
	/* 第一行是总 CPU, 格式:
	 *   cpu  user nice system idle iowait irq softirq steal guest guest_nice
	 * 我们不关心 guest* (仅虚拟化场景有意义). */
	char buf[256];
	if (fgets(buf, sizeof(buf), f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);
	memset(c, 0, sizeof(*c));
	sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
	       &c->user, &c->nice, &c->system, &c->idle,
	       &c->iowait, &c->irq, &c->softirq, &c->steal);
	return 0;
}

struct cpu_pct {
	float user, sys, iowait, idle;
};

static void cpu_stat_diff(const struct cpu_stat *prev,
                          const struct cpu_stat *cur,
                          struct cpu_pct *out)
{
	unsigned long long du = cur->user    - prev->user    + cur->nice - prev->nice;
	unsigned long long ds = cur->system  - prev->system  + cur->irq  - prev->irq
	                      + cur->softirq - prev->softirq;
	unsigned long long di = cur->idle    - prev->idle;
	unsigned long long dw = cur->iowait  - prev->iowait;
	unsigned long long total = du + ds + di + dw + (cur->steal - prev->steal);

	if (total == 0) {
		out->user = out->sys = out->iowait = 0.0f;
		out->idle = 100.0f;
		return;
	}
	out->user   = 100.0f * du / total;
	out->sys    = 100.0f * ds / total;
	out->iowait = 100.0f * dw / total;
	out->idle   = 100.0f * di / total;
}

/* ================================================================== */
/* 3) CPU 频率 — /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq  */
/* ================================================================== */

static int read_cpu_freq_mhz(int cpu)
{
	char path[128];
	long khz = 0;
	snprintf(path, sizeof(path),
	         "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
	if (read_long_from(path, &khz) != 0)
		return -1;
	return (int)(khz / 1000);
}

/* ================================================================== */
/* 4) 温度 — /sys/class/thermal/thermal_zoneN/temp                      */
/* ================================================================== */

struct therm_zone {
	char name[32];     /* 从 type 文件读, 比如 "soc-thermal" */
	float temp_c;
};

static int read_thermal(struct therm_zone *zones, int max)
{
	int count = 0;
	for (int i = 0; i < MAX_THERMAL && count < max; i++) {
		char type_path[128], temp_path[128], buf[64];
		long mC;

		snprintf(type_path, sizeof(type_path),
		         "/sys/class/thermal/thermal_zone%d/type", i);
		snprintf(temp_path, sizeof(temp_path),
		         "/sys/class/thermal/thermal_zone%d/temp", i);

		if (read_long_from(temp_path, &mC) != 0)
			continue;                                   /* zone 不存在 */

		read_first_line(type_path, buf, sizeof(buf));
		/* 简化名字: "soc-thermal" -> "soc", "gpu-thermal" -> "gpu" */
		char *dash = strchr(buf, '-');
		if (dash) *dash = '\0';
		if (!buf[0])
			snprintf(buf, sizeof(buf), "zone%d", i);

		strncpy(zones[count].name, buf, sizeof(zones[count].name) - 1);
		zones[count].name[sizeof(zones[count].name) - 1] = '\0';
		zones[count].temp_c = (float)mC / 1000.0f;
		count++;
	}
	return count;
}

/* ================================================================== */
/* 5) 网络 — /proc/net/dev 差分                                         */
/* ================================================================== */

/* 手动定义避免引入 <linux/if.h> */
#define IFNAMSIZ_LOCAL 16

struct iface_stat {
	char name[IFNAMSIZ_LOCAL];
	unsigned long long rx_bytes;
	unsigned long long tx_bytes;
};

static int read_net_dev(struct iface_stat *out, int max)
{
	FILE *f = fopen("/proc/net/dev", "r");
	if (!f) return 0;
	char line[512];
	/* 跳过两行表头 */
	if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
	if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }

	int count = 0;
	while (fgets(line, sizeof(line), f) && count < max) {
		/* 格式:   iface: rxbytes rxpkts rxerr rxdrop rxfifo rxframe rxcomp rxmcast \
		 *                txbytes txpkts ...                                         */
		char *colon = strchr(line, ':');
		if (!colon) continue;
		*colon = '\0';
		char *name = line;
		while (*name == ' ' || *name == '\t') name++;

		/* 过滤没意思的接口: lo / dummy / bond / 虚拟 bridge */
		if (strcmp(name, "lo") == 0) continue;
		if (strncmp(name, "dummy", 5) == 0) continue;

		unsigned long long rxb = 0, txb = 0;
		if (sscanf(colon + 1,
		           "%llu %*u %*u %*u %*u %*u %*u %*u %llu",
		           &rxb, &txb) < 2)
			continue;

		/* 只记有过流量的口, 否则输出太啰嗦 */
		if (rxb == 0 && txb == 0) continue;

		strncpy(out[count].name, name, sizeof(out[count].name) - 1);
		out[count].name[sizeof(out[count].name) - 1] = '\0';
		out[count].rx_bytes = rxb;
		out[count].tx_bytes = txb;
		count++;
	}
	fclose(f);
	return count;
}

/* ================================================================== */
/* 6) 进程级 CPU top5 — 遍历 /proc/<pid>/stat 差分                      */
/* ================================================================== */

struct proc_entry {
	int pid;
	char comm[COMM_LEN];
	unsigned long long ticks;   /* utime + stime */
};

/* 解析 /proc/<pid>/stat.
 * 字段 2 是 comm, 包在括号里, 可能含空格和括号, 所以要用最后一个 ')'.
 * 字段 14 (utime), 15 (stime) 分别是用户态/内核态 ticks (自进程启动累计). */
static int read_proc_stat_one(int pid, struct proc_entry *e)
{
	char path[64], buf[512];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (n == 0) return -1;
	buf[n] = '\0';

	char *lparen = strchr(buf, '(');
	char *rparen = strrchr(buf, ')');
	if (!lparen || !rparen || rparen <= lparen) return -1;
	size_t comm_len = rparen - lparen - 1;
	if (comm_len >= COMM_LEN) comm_len = COMM_LEN - 1;
	memcpy(e->comm, lparen + 1, comm_len);
	e->comm[comm_len] = '\0';

	/* rparen+1 起往后: " S ppid pgrp ... utime stime ..."
	 * 从 state (字段 3) 数到 utime (字段 14) 要跳 11 个空白字段. */
	char *p = rparen + 1;
	unsigned long long utime = 0, stime = 0;
	int rc = sscanf(p,
	                " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
	                &utime, &stime);
	if (rc < 2) return -1;

	e->pid = pid;
	e->ticks = utime + stime;
	return 0;
}

static int sample_all_procs(struct proc_entry *out, int max)
{
	DIR *d = opendir("/proc");
	if (!d) return 0;
	int count = 0;
	struct dirent *de;
	while ((de = readdir(d)) && count < max) {
		/* 只收数字目录名 */
		if (!isdigit((unsigned char)de->d_name[0])) continue;
		int pid = atoi(de->d_name);
		if (pid <= 0) continue;
		if (read_proc_stat_one(pid, &out[count]) == 0)
			count++;
	}
	closedir(d);
	return count;
}

/* 从 prev_procs 找 pid, 返回对应 ticks 或 0 (没找到就算 0 tick, 下面算 delta 会
 * 等于它当前累计值 —— 对"新起的进程"相当于从 0 算起, 合理). */
static unsigned long long find_prev_ticks(const struct proc_entry *prev,
                                          int n, int pid)
{
	for (int i = 0; i < n; i++)
		if (prev[i].pid == pid) return prev[i].ticks;
	return 0;
}

struct top_result {
	char comm[COMM_LEN];
	int pid;
	float pct;
};

/* 计算 top5. delta_ticks / (SAMPLE_SEC * clk_tck) * 100 = CPU% (单核 100% 为满).
 * 对 4 核板子, 4 个进程各占一核会显示各 ~100% (top 的约定). */
static int compute_top5(const struct proc_entry *prev, int n_prev,
                        const struct proc_entry *cur,  int n_cur,
                        struct top_result *out, int out_max,
                        long clk_tck)
{
	/* 就地组 struct 列表, 含 delta 和 name, 然后部分排序取前 N. */
	struct { unsigned long long dt; int pid; char comm[COMM_LEN]; } entries[MAX_PROCS];
	int k = 0;
	for (int i = 0; i < n_cur && k < MAX_PROCS; i++) {
		unsigned long long pt = find_prev_ticks(prev, n_prev, cur[i].pid);
		if (cur[i].ticks <= pt) continue;
		entries[k].dt = cur[i].ticks - pt;
		entries[k].pid = cur[i].pid;
		strncpy(entries[k].comm, cur[i].comm, COMM_LEN);
		entries[k].comm[COMM_LEN - 1] = '\0';
		k++;
	}
	if (k == 0) return 0;

	/* 选择排序前 N (k 最多 ~300, N=5, O(k*N) = 1500 次比较, 可忽略) */
	int want = (k < out_max) ? k : out_max;
	for (int i = 0; i < want; i++) {
		int best = i;
		for (int j = i + 1; j < k; j++)
			if (entries[j].dt > entries[best].dt) best = j;
		if (best != i) {
			__typeof__(entries[0]) tmp = entries[i];
			entries[i] = entries[best];
			entries[best] = tmp;
		}
		out[i].pid = entries[i].pid;
		strncpy(out[i].comm, entries[i].comm, COMM_LEN);
		out[i].comm[COMM_LEN - 1] = '\0';
		out[i].pct = 100.0f * (float)entries[i].dt / ((float)SAMPLE_SEC * (float)clk_tck);
	}
	return want;
}

/* ================================================================== */
/* 7) uptime 格式化                                                     */
/* ================================================================== */

static double read_uptime_sec(void)
{
	FILE *f = fopen("/proc/uptime", "r");
	double up = 0.0;
	if (f) {
		if (fscanf(f, "%lf", &up) != 1) up = 0.0;
		fclose(f);
	}
	return up;
}

static void format_uptime(double up, char *buf, size_t cap)
{
	long total = (long)up;
	long days = total / 86400; total %= 86400;
	long hrs  = total / 3600;  total %= 3600;
	long mins = total / 60;    long secs = total % 60;
	snprintf(buf, cap, "%ldd:%02ld:%02ld:%02ld", days, hrs, mins, secs);
}

/* ================================================================== */
/* 主线程                                                               */
/* ================================================================== */

void *sys_stat_thread(void *arg)
{
	int prev_idx, slot;
	char logpath[128];
	FILE *out;
	unsigned long written = 0;
	long clk_tck;

	(void)arg;

	mkdir(LOG_DIR, 0755);

	/* slot 轮换: 读出上次的编号, 本次 +1, 超 MAX_SLOTS 回到 1. */
	prev_idx = read_index();
	slot = prev_idx + 1;
	if (slot > MAX_SLOTS) slot = 1;

	snprintf(logpath, sizeof(logpath), LOG_DIR "/stat.%d", slot);
	unlink(logpath);            /* 同一 slot 覆盖, 保留上一次的对比参考 */

	out = fopen(logpath, "w");
	if (!out) {
		LOGE("open %s: %s", logpath, strerror(errno));
		return NULL;
	}
	setvbuf(out, NULL, _IOLBF, 0);

	write_index(slot);

	clk_tck = sysconf(_SC_CLK_TCK);
	if (clk_tck <= 0) clk_tck = 100;   /* 兜底, Linux 默认 100 Hz */

	/* ========== 文件头: 写一次, 说明字段含义 ========== */
	{
		time_t t = time(NULL);
		struct tm lt;
		char tbuf[64];
		localtime_r(&t, &lt);
		strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);

		written += fprintf(out,
		    "==== sys_stat slot=%d  start=%s  (epoch=%ld)  CLK_TCK=%ld ====\n",
		    slot, tbuf, (long)t, clk_tck);
		written += fprintf(out,
		    "# 字段速查:\n"
		    "#   uptime   自内核启动到现在的时间 (与系统时钟无关, 始终准)\n"
		    "#   LOAD     load1/load5 (runqueue 长度平均, 不是 CPU%%)\n"
		    "#   CPU      user/sys/iowait/idle%% (两次采样差分, 同 top)\n"
		    "#   FREQ     每核当前频率 MHz (来源 cpufreq scaling_cur_freq)\n"
		    "#   TEMP     各 thermal_zone 温度 °C\n"
		    "#   MEM      total/avail/cached KiB + used%% (used 按 avail 算)\n"
		    "#   NET      每口 rx/tx KB/s (差分, 来源 /proc/net/dev)\n"
		    "#   TOP5     CPU 耗时增量最高的 5 个进程 (COMM[PID]=%%CPU)\n"
		    "# 采样周期 %d 秒, 单文件上限 %lu MiB, 达到上限后停止写入.\n\n",
		    SAMPLE_SEC, SIZE_CAP / (1024UL * 1024UL));
		fflush(out);
	}

	/* ========== 初始快照 (作为第一次差分的基线) ========== */
	struct cpu_stat prev_cpu;
	read_cpu_stat(&prev_cpu);

	struct iface_stat prev_if[MAX_IFACES];
	int n_prev_if = read_net_dev(prev_if, MAX_IFACES);

	struct proc_entry *prev_procs = calloc(MAX_PROCS, sizeof(*prev_procs));
	struct proc_entry *cur_procs  = calloc(MAX_PROCS, sizeof(*cur_procs));
	if (!prev_procs || !cur_procs) {
		LOGE("OOM on proc tables");
		fclose(out);
		free(prev_procs); free(cur_procs);
		return NULL;
	}
	int n_prev_procs = sample_all_procs(prev_procs, MAX_PROCS);

	/* 第一次等一个周期, 否则差分全是满值 */
	sleep(SAMPLE_SEC);

	/* ========== 主循环 ========== */
	for (;;) {
		if (written >= SIZE_CAP) {
			/* 文件满了就挂机, 不再写, 避免填爆 /blackbox */
			sleep(SAMPLE_SEC);
			continue;
		}

		/* ---- 1. 时间 & uptime ---- */
		time_t now = time(NULL);
		struct tm lt;
		char tbuf[32], ubuf[32];
		localtime_r(&now, &lt);
		strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);
		format_uptime(read_uptime_sec(), ubuf, sizeof(ubuf));

		/* ---- 2. load ---- */
		char load[128]; float l1 = 0, l5 = 0;
		read_first_line("/proc/loadavg", load, sizeof(load));
		sscanf(load, "%f %f", &l1, &l5);

		/* ---- 3. CPU 分解 ---- */
		struct cpu_stat cur_cpu; struct cpu_pct pct = {0};
		if (read_cpu_stat(&cur_cpu) == 0)
			cpu_stat_diff(&prev_cpu, &cur_cpu, &pct);
		prev_cpu = cur_cpu;

		/* ---- 4. CPU 频率 ---- */
		int freqs[NUM_CPUS];
		for (int i = 0; i < NUM_CPUS; i++)
			freqs[i] = read_cpu_freq_mhz(i);

		/* ---- 5. 温度 ---- */
		struct therm_zone zones[MAX_THERMAL];
		int n_zones = read_thermal(zones, MAX_THERMAL);

		/* ---- 6. 内存 ---- */
		struct meminfo mi; read_meminfo(&mi);
		float used_pct = 0.0f;
		if (mi.total_kb > 0) {
			unsigned long used = (mi.avail_kb < mi.total_kb) ?
			                     (mi.total_kb - mi.avail_kb) : 0;
			used_pct = 100.0f * (float)used / (float)mi.total_kb;
		}

		/* ---- 7. 网络差分 ---- */
		struct iface_stat cur_if[MAX_IFACES];
		int n_cur_if = read_net_dev(cur_if, MAX_IFACES);

		/* ---- 8. 进程 top5 ---- */
		int n_cur_procs = sample_all_procs(cur_procs, MAX_PROCS);
		struct top_result top[TOP_N];
		int n_top = compute_top5(prev_procs, n_prev_procs,
		                         cur_procs,  n_cur_procs,
		                         top, TOP_N, clk_tck);

		/* ================== 输出这一块 ================== */

		written += fprintf(out, "[%s] uptime=%s\n", tbuf, ubuf);

		written += fprintf(out, "  LOAD : load1=%.2f  load5=%.2f\n", l1, l5);

		written += fprintf(out,
		    "  CPU  : user=%.1f%%  sys=%.1f%%  iowait=%.1f%%  idle=%.1f%%\n",
		    pct.user, pct.sys, pct.iowait, pct.idle);

		written += fprintf(out, "  FREQ :");
		for (int i = 0; i < NUM_CPUS; i++) {
			if (freqs[i] < 0)
				written += fprintf(out, "  cpu%d=?", i);
			else
				written += fprintf(out, "  cpu%d=%d", i, freqs[i]);
		}
		written += fprintf(out, "  (MHz)\n");

		if (n_zones > 0) {
			written += fprintf(out, "  TEMP :");
			for (int i = 0; i < n_zones; i++)
				written += fprintf(out, "  %s=%.1fC",
				                   zones[i].name, zones[i].temp_c);
			written += fprintf(out, "\n");
		}

		written += fprintf(out,
		    "  MEM  : total=%luKB  avail=%luKB  cached=%luKB  used=%.1f%%\n",
		    mi.total_kb, mi.avail_kb, mi.cached_kb, used_pct);

		/* 网络差分 */
		if (n_cur_if > 0) {
			written += fprintf(out, "  NET  :");
			int printed = 0;
			for (int i = 0; i < n_cur_if; i++) {
				/* 找同名前样 */
				unsigned long long pr = 0, pt = 0;
				for (int j = 0; j < n_prev_if; j++) {
					if (strcmp(cur_if[i].name, prev_if[j].name) == 0) {
						pr = prev_if[j].rx_bytes;
						pt = prev_if[j].tx_bytes;
						break;
					}
				}
				double rxkb = 0, txkb = 0;
				if (cur_if[i].rx_bytes >= pr)
					rxkb = (double)(cur_if[i].rx_bytes - pr) / SAMPLE_SEC / 1024.0;
				if (cur_if[i].tx_bytes >= pt)
					txkb = (double)(cur_if[i].tx_bytes - pt) / SAMPLE_SEC / 1024.0;
				/* 两个方向都是 0 就不打, 屏掉没流量的口 */
				if (rxkb < 0.01 && txkb < 0.01) continue;
				if (printed) written += fprintf(out, "  |");
				written += fprintf(out, "  %s rx=%.1f/tx=%.1f",
				                   cur_if[i].name, rxkb, txkb);
				printed++;
			}
			if (!printed) written += fprintf(out, "  (idle)");
			written += fprintf(out, "  (KB/s)\n");
		}

		if (n_top > 0) {
			written += fprintf(out, "  TOP5 :");
			for (int i = 0; i < n_top; i++) {
				if (top[i].pct < 0.05f) break;   /* 连 0.1% 都不到就别啰嗦 */
				written += fprintf(out, "  %s[%d]=%.1f%%",
				                   top[i].comm, top[i].pid, top[i].pct);
			}
			written += fprintf(out, "\n");
		}

		written += fprintf(out, "\n");
		fflush(out);

		/* 保存基线用于下轮差分 */
		memcpy(prev_if, cur_if, sizeof(cur_if));
		n_prev_if = n_cur_if;
		{
			struct proc_entry *swap = prev_procs;
			prev_procs = cur_procs;
			cur_procs = swap;
			n_prev_procs = n_cur_procs;
		}

		sleep(SAMPLE_SEC);
	}

	/* 不可达, 但保持形式完整 */
	fclose(out);
	free(prev_procs);
	free(cur_procs);
	return NULL;
}
