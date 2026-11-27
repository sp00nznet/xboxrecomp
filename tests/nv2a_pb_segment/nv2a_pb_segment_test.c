/* The segment scan, the default walk: nv2a_pb_scan_segment() reads the bytes
 * between two PUTs and follows CALLs out of them.
 *
 * The guest memory is the top of the contiguous window: a small buffer whose
 * last byte is the window's last byte (0x83FFFFFF), followed by a page that
 * faults, as the host memory past the window can. The executor is stubbed and
 * counts the methods it is given. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define WINDOW_END 0x84000000u
#define MEM_BYTES  0x10000u
#define PAGE       0x1000u

static uint8_t *s_mem;                      /* guest WINDOW_END - MEM_BYTES */

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return (ptrdiff_t)s_mem - (ptrdiff_t)(WINDOW_END - MEM_BYTES);
}

volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

static int s_n;
static uint32_t s_last_method, s_last_param;

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    (void)subch;
    s_n++;
    s_last_method = method;
    s_last_param = param;
}
int nv2a_pb_exec_constant_word(uint32_t method, uint32_t param)
{
    (void)method; (void)param;
    return 0;
}
void nv2a_pb_exec_report(void) {}
void xbox_Nv2aAckBusyBits(void) {}
void nv2a_pb_exec_walk_begin(void) {}

void nv2a_pb_scan_segment(uint32_t start_va, uint32_t end_va);
int nv2a_pb_scan_wrap(uint32_t start_va, uint32_t put_va);

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static void put(uint32_t va, uint32_t w)
{
    memcpy(s_mem + (va - (WINDOW_END - MEM_BYTES)), &w, 4);
}

static uint32_t hdr(uint32_t count, uint32_t subch, uint32_t method)
{
    return (count << 18) | (subch << 13) | method;
}

/* The buffer, with a page after it that faults on any access. */
static uint8_t *alloc_with_guard(void)
{
#ifdef _WIN32
    DWORD old;
    uint8_t *p = VirtualAlloc(NULL, MEM_BYTES + PAGE, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    if (!p || !VirtualProtect(p + MEM_BYTES, PAGE, PAGE_NOACCESS, &old))
        return NULL;
#else
    uint8_t *p = mmap(NULL, MEM_BYTES + PAGE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || mprotect(p + MEM_BYTES, PAGE, PROT_NONE))
        return NULL;
#endif
    return p;
}

int main(void)
{
#ifdef _WIN32
    _putenv_s("RECOMP_PB_EXEC", "1");
#else
    setenv("RECOMP_PB_EXEC", "1", 1);
#endif
    s_mem = alloc_with_guard();
    if (!s_mem) {
        printf("FAIL: no guarded buffer\n");
        return 1;
    }

    /* A CALL to the window's last 16 bytes: one method and no RETURN, as
     * when the walk reads data as commands. The scan follows it to the end of
     * the window and stops there, instead of reading on into the page after
     * it (without the bound this faults). */
    {
        const uint32_t seg = WINDOW_END - 0x100u, sub = WINDOW_END - 0x10u;

        put(seg, sub | 2u);                                /* CALL sub */
        put(seg + 4, hdr(1, 0, 0x0100)); put(seg + 8, 7);
        put(sub, hdr(1, 0, 0x0200)); put(sub + 4, 5);      /* no RETURN */
        nv2a_pb_scan_segment(seg, seg + 12);
        CHECK(s_n == 2);
        CHECK(s_last_method == 0x0100 && s_last_param == 7);
    }

    /* A CALL whose target is past the window (here its first byte, the
     * page that faults) is not followed. */
    {
        const uint32_t seg = WINDOW_END - 0x200u;
        int n0 = s_n;

        put(seg, (WINDOW_END & 0x0FFFFFFFu) | 2u);         /* CALL 0x84000000 */
        put(seg + 4, hdr(1, 0, 0x0104)); put(seg + 8, 9);
        nv2a_pb_scan_segment(seg, seg + 12);
        CHECK(s_n == n0 + 1);
        CHECK(s_last_method == 0x0104 && s_last_param == 9);
    }

    /* PUT wraps: the title wrote one method at the last PUT and a JUMP back
     * to the ring's start, then one method there. The scan runs the first,
     * follows the JUMP and runs the second. */
    {
        const uint32_t ring = WINDOW_END - 0x8000u, last = ring + 0x100u;
        int n0 = s_n;

        put(last, hdr(1, 0, 0x0304)); put(last + 4, 2);
        put(last + 8, ring | 1u);                          /* JUMP ring */
        put(ring, hdr(1, 0, 0x0300)); put(ring + 4, 1);
        CHECK(nv2a_pb_scan_wrap(last, ring + 8) == 1);
        CHECK(s_n == n0 + 2);
        CHECK(s_last_method == 0x0300 && s_last_param == 1);
    }

    /* PUT moved down with no JUMP before the window's end (a reset, a new
     * device): nothing runs, and the caller falls back. The method the
     * look-ahead meets (the CALL target above) is stale. */
    {
        int n0 = s_n;

        CHECK(nv2a_pb_scan_wrap(WINDOW_END - 0x40u, WINDOW_END - 0x8000u) == 0);
        CHECK(s_n == n0);
    }

    /* A stale JUMP whose target is above PUT is not the wrap: nothing runs. */
    {
        const uint32_t ring = WINDOW_END - 0x6000u, last = ring + 0x100u;
        int n0 = s_n;

        put(last, hdr(1, 0, 0x0308)); put(last + 4, 3);
        put(last + 8, (ring + 0x200u) | 1u);               /* JUMP past PUT */
        CHECK(nv2a_pb_scan_wrap(last, ring + 8) == 0);
        CHECK(s_n == n0);
    }

    if (!s_fail)
        printf("nv2a pushbuffer segment scan: ok\n");
    return s_fail;
}
