/*
 * Win32 HANDLEs on a POSIX host must be representable in the guest.
 *
 * A recompiled title is 32-bit code. Every HANDLE it receives is stored in a
 * 32-bit slot -- in a guest struct, on the guest stack, in a kernel object
 * field -- and handed back later. The kernel bridge does this literally:
 *
 *     sp[1] = (uint32_t)(uintptr_t)h;
 *
 * On Windows that is free. A Win32 HANDLE is an index into the process handle
 * table, small even in a 64-bit process, which is why Microsoft documents
 * handles as 32-bit-significant for interop.
 *
 * The POSIX shim instead returns the w32_object pointer itself, and macOS heap
 * pointers sit above 4 GB -- 0x100cd3000 in the run that prompted this. The
 * truncation is silent: the guest stores 0x00cd3000, hands it back, and the
 * shim dereferences an address that was never an object. Nothing reports a bad
 * handle, because from the shim's side it is just a pointer.
 *
 * tests/kernel_directory is where this first showed up: it guards its own
 * setup with `(uintptr_t)h > UINT32_MAX` and exits 11 rather than running.
 *
 * Two properties per handle type, because fitting and surviving are different
 * claims: the value must fit in 32 bits, and a handle actually put through
 * that truncation must still work.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-52s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* What the guest does to a handle: keep the low 32 bits, nothing else. */
static HANDLE through_guest(HANDLE h)
{
    return (HANDLE)(uintptr_t)(uint32_t)(uintptr_t)h;
}

/* The round-trip checks run in a child. Using a truncated handle dereferences
 * an address that was never an object, so the failure is a SIGSEGV rather than
 * a return value -- in-process it would take the whole run with it and report
 * nothing about the handle types after it. The child inherits the handle
 * across fork, so it is the same object either way. */
enum use { USE_EVENT, USE_SEM, USE_FILE, USE_DUP_BAD, USE_WAIT_BAD,
           USE_CLOSE_BAD };

static int roundtrip_in_child(enum use what, HANDLE h, char *how, size_t n)
{
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        HANDLE g = through_guest(h);
        int ok = 0;
        switch (what) {
        case USE_EVENT: ok = SetEvent(g) ? 1 : 0; break;
        case USE_SEM:   ok = WaitForSingleObject(g, 0) == WAIT_OBJECT_0; break;
        case USE_FILE: {
            DWORD wrote = 0;
            ok = WriteFile(g, "xbr", 3, &wrote, NULL) && wrote == 3;
            break;
        }
        /* The bad-handle cases below pass `h` straight through rather than
         * truncating it: the handle is already bogus, and what is under test
         * is that the shim rejects it instead of dereferencing it. */
        case USE_DUP_BAD: {
            HANDLE out = NULL;
            ok = !DuplicateHandle(GetCurrentProcess(), h,
                                  GetCurrentProcess(), &out, 0, FALSE, 0);
            break;
        }
        case USE_WAIT_BAD:
            ok = WaitForSingleObject(h, 0) == WAIT_FAILED;
            break;
        case USE_CLOSE_BAD:
            CloseHandle(h);
            ok = 1;            /* must simply not crash */
            break;
        }
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        snprintf(how, n, "died from signal %d (%s)",
                 WTERMSIG(status), strsignal(WTERMSIG(status)));
        return 0;
    }
    if (WEXITSTATUS(status) != 0) {
        snprintf(how, n, "the call reported failure");
        return 0;
    }
    return 1;
}

static void check_fits(HANDLE h, const char *what)
{
    char detail[128];
    snprintf(detail, sizeof detail,
             "handle is 0x%llx, which truncates to 0x%08X in a guest slot",
             (unsigned long long)(uintptr_t)h, (uint32_t)(uintptr_t)h);
    check((uintptr_t)h <= UINT32_MAX, what, detail);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1. An event handle. */
    {
        HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
        check(e != NULL, "CreateEventA returns a handle", NULL);
        if (e) {
            check_fits(e, "event handle fits in 32 bits");
            /* And the truncated value must still name the same object. */
            char how[160] = "";
            check(roundtrip_in_child(USE_EVENT, e, how, sizeof how),
                  "event survives a round trip through a guest slot", how);
            CloseHandle(e);
        }
    }

    /* 2. A semaphore handle. */
    {
        HANDLE s = CreateSemaphoreA(NULL, 1, 1, NULL);
        check(s != NULL, "CreateSemaphoreA returns a handle", NULL);
        if (s) {
            check_fits(s, "semaphore handle fits in 32 bits");
            char how[160] = "";
            check(roundtrip_in_child(USE_SEM, s, how, sizeof how),
                  "semaphore survives a round trip through a guest slot", how);
            CloseHandle(s);
        }
    }

    /* 3. A mutex handle. */
    {
        HANDLE m = CreateMutexA(NULL, FALSE, NULL);
        check(m != NULL, "CreateMutexA returns a handle", NULL);
        if (m) {
            check_fits(m, "mutex handle fits in 32 bits");
            CloseHandle(m);
        }
    }

    /* 4. A file handle, and a directory handle -- the case kernel_directory
     *    needs, and the one that is passed straight into guest memory. */
    {
        char root[MAX_PATH], dir[MAX_PATH], file[MAX_PATH];
        GetTempPathA(MAX_PATH, root);
        snprintf(dir, sizeof dir, "%sxbr-handle-test-%lu",
                 root, (unsigned long)GetCurrentProcessId());
        if (!CreateDirectoryA(dir, NULL)) {
            check(0, "temp directory created", "CreateDirectoryA failed");
        } else {
            snprintf(file, sizeof file, "%s/probe.bin", dir);
            HANDLE f = CreateFileA(file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            check(f != INVALID_HANDLE_VALUE, "CreateFileA returns a handle", NULL);
            if (f != INVALID_HANDLE_VALUE) {
                check_fits(f, "file handle fits in 32 bits");
                char how[160] = "";
                check(roundtrip_in_child(USE_FILE, f, how, sizeof how),
                      "file survives a round trip through a guest slot", how);
                CloseHandle(f);
            }

            HANDLE d = CreateFileA(dir, FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, NULL);
            check(d != INVALID_HANDLE_VALUE,
                  "CreateFileA opens a directory", NULL);
            if (d != INVALID_HANDLE_VALUE) {
                check_fits(d, "directory handle fits in 32 bits");
                CloseHandle(d);
            }
            remove(file);
            RemoveDirectoryA(dir);
        }
    }

    /* 5. Slots must be recycled.
     *
     * An index-based handle is a finite resource in a way a pointer is not.
     * A title that opens and closes handles in a loop -- every file read, every
     * event wait -- would walk the table off the end within minutes if
     * CloseHandle did not free the slot. Checked by watching the values rather
     * than the table, which is private: with recycling they stay near the low
     * water mark, without it they climb without bound. */
    {
        HANDLE first = NULL, last = NULL;
        int made = 0;
        for (int i = 0; i < 10000; i++) {
            HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
            if (!e) break;
            if (!first) first = e;
            last = e;
            made++;
            CloseHandle(e);
        }
        check(made == 10000, "10000 create/close cycles all succeed",
              "handle allocation started failing");

        char detail[128];
        snprintf(detail, sizeof detail,
                 "first handle 0x%llx, ten-thousandth 0x%llx",
                 (unsigned long long)(uintptr_t)first,
                 (unsigned long long)(uintptr_t)last);
        check(last == first, "a closed handle's slot is reused", detail);
    }

    /* 6. A handle the shim never issued must be rejected, not dereferenced.
     *
     * Now that a HANDLE is a table index, an unknown one resolves to NULL
     * rather than to a garbage pointer -- so every caller that used to get
     * away with `(w32_object *)h` has to check. Burnout 3 found the one that
     * did not: NtDuplicateObject on a stale handle reached
     * InterlockedIncrement(&o->refcount) with o == NULL and faulted at
     * address 0x4, refcount being the second field.
     *
     * A guest supplies these values, so "no handle the shim issued looks like
     * this" is not a guarantee available to us. Each runs in a child. */
    {
        const struct { enum use what; const char *name; } bad[] = {
            { USE_DUP_BAD,   "DuplicateHandle rejects an unknown handle" },
            { USE_WAIT_BAD,  "WaitForSingleObject rejects an unknown handle" },
            { USE_CLOSE_BAD, "CloseHandle tolerates an unknown handle" },
        };
        /* Well-formed for the table -- a multiple of the stride, far past any
         * live slot -- which is exactly the shape a stale guest handle has. */
        HANDLE bogus = (HANDLE)(uintptr_t)0x4000u;
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            char how[160] = "";
            check(roundtrip_in_child(bad[i].what, bogus, how, sizeof how),
                  bad[i].name, how);
        }
    }

    /* 7. The producers that do not look like one.
     *
     * Converting the shim to table handles means finding every place an
     * object becomes a HANDLE, and two of them do not match a grep for a
     * returned object: GetCurrentThread hands back the thread-local self
     * pointer, and CreateTimerQueueTimer writes through an out-parameter.
     * Both leaked a raw pointer when this was first done. The second had no
     * test at all; the first was caught only because win32_thread_posix
     * compares o == t_self_obj to decide whether a self-suspend parks, so an
     * idle worker pool span instead of sleeping. Assert them directly. */
    {
        HANDLE me = GetCurrentThread();
        check(me != NULL, "GetCurrentThread returns a handle", NULL);
        if (me && me != (HANDLE)(LONG_PTR)-2) {
            check_fits(me, "GetCurrentThread's handle fits in 32 bits");
            check(GetCurrentThread() == me,
                  "GetCurrentThread is stable across calls",
                  "an object must keep one id for its lifetime, or a guest "
                  "that stored the first value cannot match the second");
        }

        HANDLE t = NULL;
        if (CreateTimerQueueTimer(&t, NULL, NULL, NULL, 10, 0, 0) && t)
            check_fits(t, "CreateTimerQueueTimer's out-parameter fits in 32 bits");
        else
            check(1, "CreateTimerQueueTimer unavailable; skipped", NULL);
    }

    /* 8. A handle's remembered path survives the round trip.
     *
     * The POSIX kernel resolves a relative open -- a name given against an
     * OBJECT_ATTRIBUTES RootDirectory, which is how the save API reaches each
     * slot entry -- by asking the shim for the path the directory handle was
     * opened with. The Win32 backend instead calls GetFinalPathNameByHandleW,
     * which the shim does not have. So this lookup IS the POSIX
     * implementation of that resolution, and it is reached with a handle the
     * guest stored and handed back. A truncated value finds no object and the
     * open fails with a path error, which is what kernel_object_paths saw.
     *
     * Separators: Win32 takes either, so a path built as "<dir>\\child" must
     * reach the same file as "<dir>/child". Without that translation the
     * backslash is an ordinary filename character on POSIX and the call
     * silently acts on a sibling whose name contains it. */
    {
        char root[MAX_PATH], dir[MAX_PATH], child[MAX_PATH];
        GetTempPathA(MAX_PATH, root);
        snprintf(dir, sizeof dir, "%sxbr-handle-path-%lu",
                 root, (unsigned long)GetCurrentProcessId());
        rmdir(dir);
        check(CreateDirectoryA(dir, NULL) != 0,
              "CreateDirectoryA creates a directory", NULL);

        snprintf(child, sizeof child, "%s\\Child", dir);
        if (CreateDirectoryA(child, NULL)) {
            char expect[MAX_PATH];
            snprintf(expect, sizeof expect, "%s/Child", dir);
            struct stat st;
            check(stat(expect, &st) == 0 && S_ISDIR(st.st_mode),
                  "CreateDirectoryA treats a backslash as a separator",
                  "a literal backslash in the name instead of a child dir");
            /* INVALID_FILE_ATTRIBUTES is 0xFFFFFFFF, so it satisfies any
             * mask test -- the failure has to be excluded first. */
            DWORD at = GetFileAttributesA(child);
            check(at != INVALID_FILE_ATTRIBUTES &&
                  (at & FILE_ATTRIBUTE_DIRECTORY),
                  "GetFileAttributesA accepts a backslash separator", NULL);
            rmdir(expect);
        }

        HANDLE d = CreateFileA(dir, FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (d != INVALID_HANDLE_VALUE) {
            const char *via = w32_handle_path(through_guest(d));
            check(via && strcmp(via, dir) == 0,
                  "a handle's path survives the round trip",
                  via ? "wrong path came back" :
                        "the truncated handle resolved to no object, so a "
                        "relative open against it cannot find its directory");
            CloseHandle(d);
        }
        rmdir(dir);
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
