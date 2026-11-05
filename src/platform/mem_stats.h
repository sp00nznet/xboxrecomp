/*
 * mem_stats.h - what the running game costs in memory, for the bench.
 *
 * Bench and golden runs measure frame speed; this adds the process's RAM
 * and the GPU memory the backend holds, so a run that grows (a cache, a
 * texture leak) shows up beside its flips/s. One line at exit, always:
 *
 *   [MEM] exit ram_mib=812.4 ram_peak_mib=905.1 commit_mib=n/a ... why=atexit
 *
 * With the RAM cache in use the line also has ramcache_mib and
 * ramcache_peak_mib, after gpu_nl_peak_mib.
 *
 * key=value pairs, MiB with one decimal, n/a for what the host cannot give
 * (the CLI's benchdb parses it; mem_stats_parse is the reference reading).
 * RECOMP_TRACE=mem=S adds a "[MEM] sample t=..." line every S seconds.
 *
 * RAM: macOS phys_footprint and its ledger peak (task_info TASK_VM_INFO),
 * Windows and Wine the working set, its peak and the private commit
 * (K32GetProcessMemoryInfo), Linux VmRSS and VmHWM (/proc/self/status).
 * On macOS the GPU shares the memory and phys_footprint counts the Metal
 * allocations too, so there ram_* already holds the gpu_* figure; add
 * them only on a host with its own video memory. Under Wine the commit's
 * "peak" is the current value (Wine keeps no peak), so commit_peak_mib is
 * not a peak under Proton.
 * GPU: the backend calls mem_stats_gpu_note with what its API reports
 * (Metal currentAllocatedSize, D3D11 QueryVideoMemoryInfo); the line has
 * the last note and the largest, so a peak between notes can be missed.
 *
 * Benches stop the game with SIGINT and the Win32 window with ExitProcess,
 * and neither reaches atexit, so the exit line also prints from a SIGINT /
 * SIGTERM handler (POSIX), a console control handler (Windows; Wine turns
 * SIGINT into CTRL_C_EVENT) and the window exit hook. Whichever comes
 * first prints it; the others do nothing.
 */
#ifndef MEM_STATS_H
#define MEM_STATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A value the host or backend cannot give. */
#define MEM_STATS_NA UINT64_MAX

/* One reading, in bytes; MEM_STATS_NA where unknown. */
struct mem_stats {
    uint64_t ram, ram_peak;
    uint64_t commit, commit_peak;
    uint64_t gpu, gpu_peak;
    uint64_t gpu_nl, gpu_nl_peak;
    /* The cache partition in memory (kernel_ramfs.h), charged in 16 KB
     * clusters, seed included: part of ram, not on top of it. n/a while
     * the store is off or unused, and then the line leaves both out. */
    uint64_t ramcache, ramcache_peak;
    const char *ram_src;   /* "phys_footprint", "working_set", "rss", "none" */
    const char *gpu_src;   /* "metal", "d3d11", "none" */
};

/* Installs the exit paths (once) and, with RECOMP_TRACE=mem=S, the
 * periodic sampler. Any thread; call once early (process-main). */
void mem_stats_start_from_env(void);

/* The backend's GPU figures in bytes (MEM_STATS_NA for one it cannot
 * give); src names the API ("metal", "d3d11"). Cheap: a few atomic
 * stores. Any thread. */
void mem_stats_gpu_note(const char *src, uint64_t local, uint64_t nonlocal);

/* The RAM cache's charge in bytes, each time it changes. Cheap: two
 * atomic stores. Any thread. */
void mem_stats_ramcache_note(uint64_t used);

/* The process's figures now, plus the GPU's last note. Returns 0 when the
 * host gives no RAM figures (ram fields n/a). Async-signal-safe on POSIX
 * (a Mach call or open/read of /proc), so the signal path can use it. */
int mem_stats_read(struct mem_stats *out);

/* The line without "[MEM] " and without a newline: "<kind> [t=..] k=v ...
 * why=<why>" (t only when t_ms >= 0; why only when non-NULL). Integer
 * arithmetic and no stdio, so it is async-signal-safe. Returns the length
 * written (always NUL-terminated; truncated when n is too small). */
size_t mem_stats_format(char *buf, size_t n, const char *kind, int64_t t_ms,
                        const struct mem_stats *m, const char *why);

/* Reads a formatted line (with or without the "[MEM] " prefix) back into m:
 * the byte fields it names, rounded to the 0.1 MiB printed; fields not in
 * the line, or n/a, are MEM_STATS_NA. kind gets the first word (up to
 * kind_n - 1 chars). Returns the number of byte fields read, -1 when the
 * line is not a [MEM] line. For the ctest and any C reader. */
int mem_stats_parse(const char *line, char *kind, size_t kind_n, struct mem_stats *m);

/* The sample period RECOMP_TRACE=mem=S asks for, in ms: 0 (off) for NULL,
 * empty, 0, a negative or a non-number; at least 100 (0.1 s). */
unsigned mem_stats_period_ms(const char *v);

/* Prints the exit line now (once per process); why says which path. */
void mem_stats_exit_line(const char *why);

#ifdef __cplusplus
}
#endif

#endif /* MEM_STATS_H */
