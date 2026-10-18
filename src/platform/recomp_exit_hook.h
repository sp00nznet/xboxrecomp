/*
 * recomp_exit_hook.h - work that must run when a window ends the process.
 *
 * The Win32 windows end the run with ExitProcess, which skips atexit. The
 * kernel's exit-time reports (the missing-file summary) register here, and
 * each window calls recomp_exit_hook_run before ExitProcess. It lives in the
 * platform layer because the D3D library is linked without the kernel in
 * its standalone tests, so it cannot call the kernel by name.
 */
#ifndef RECOMP_EXIT_HOOK_H
#define RECOMP_EXIT_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*recomp_exit_hook_fn)(const char *why);

/* One hook; a second call replaces the first. Any thread. */
void recomp_exit_hook_set(recomp_exit_hook_fn fn);

/* More hooks that run after it, for state outside the process that the OS
 * may not undo on a Wine exit (the mouse's cursor clip). Up to four; adding
 * one twice keeps one. Returns 0 when the table is full. Any thread. */
int recomp_exit_hook_add(recomp_exit_hook_fn fn);

/* Calls the hook, if one is set, then the added ones, with why ("window"). */
void recomp_exit_hook_run(const char *why);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_EXIT_HOOK_H */
