/* PUSH32/POP32 against the x86 rules for stack operands that alias ESP.
 *
 * The lifter emits `pop esp` as POP32(esp, esp). x86 increments ESP and then
 * writes the popped value, so ESP ends up equal to it; writing first and
 * incrementing after left ESP 4 bytes high. Spartan's sub_001597A0
 * (push esp; push eax; fld; frndint; fistp [esp]; pop eax; pop esp) ran that
 * in a loop, and the function's closing pops then read the wrong slots
 *. Each case below is the exact statement sequence the lifter emits
 * for the instruction named in its comment, run over a fake guest stack.
 */
#include "recomp_types.h"
#include <stdio.h>
#include <string.h>

ptrdiff_t g_xbox_mem_offset;

static uint32_t stack_mem[64];
static unsigned failures;

#define STACK_VA 0x00F00000u
#define TOP (STACK_VA + (uint32_t)sizeof stack_mem - 16u)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
    ++failures; } } while (0)

int main(void)
{
    uint32_t esp, eax, ebp, edi, esi, ebx, edx, ecx;

    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)stack_mem - STACK_VA);

    /* push esp stores the value ESP had before the push. */
    esp = TOP;
    PUSH32(esp, esp);
    CHECK(esp == TOP - 4);
    CHECK(MEM32(esp) == TOP);

    /* push esp; pop esp leaves ESP where it was. */
    esp = TOP;
    PUSH32(esp, esp);
    POP32(esp, esp);
    CHECK(esp == TOP);

    /* The round-to-int idiom: push esp; push eax; (fistp [esp]); pop eax;
     * pop esp. ESP comes back unchanged and eax holds the stored integer. */
    esp = TOP;
    eax = 0xDEADBEEFu;
    PUSH32(esp, esp);
    PUSH32(esp, eax);
    MEM32(esp) = 42u;  /* fistp dword [esp] */
    POP32(esp, eax);
    POP32(esp, esp);
    CHECK(esp == TOP);
    CHECK(eax == 42u);

    /* pop esp loads whatever the slot holds, not slot + 4. */
    esp = TOP;
    PUSH32(esp, 0x00F00040u);
    POP32(esp, esp);
    CHECK(esp == 0x00F00040u);

    /* An ordinary pop: value out, ESP up by 4. */
    esp = TOP;
    PUSH32(esp, 0x12345678u);
    POP32(esp, eax);
    CHECK(eax == 0x12345678u);
    CHECK(esp == TOP);

    /* push dword [esp+4] reads the operand before ESP moves. */
    esp = TOP;
    MEM32(esp + 4) = 0xA5A5A5A5u;
    PUSH32(esp, MEM32(esp + 4));
    CHECK(MEM32(esp) == 0xA5A5A5A5u);

    /* pop dword [esp+4]: the SDM computes the address after the increment.
     * The lifter pops into _tmp first, then writes [esp+4] with the new ESP. */
    esp = TOP - 8;
    MEM32(TOP - 8) = 0x0BADF00Du;
    MEM32(TOP - 4) = 0u;
    MEM32(TOP) = 0u;
    { uint32_t _tmp; POP32(esp, _tmp); MEM32(esp + 4) = _tmp; }
    CHECK(esp == TOP - 4);
    CHECK(MEM32(TOP) == 0x0BADF00Du);
    CHECK(MEM32(TOP - 4) == 0u);

    /* leave: esp = ebp; pop ebp. */
    esp = TOP - 32;
    ebp = TOP - 8;
    MEM32(ebp) = 0xCAFEF00Du;
    esp = ebp;
    POP32(esp, ebp);
    CHECK(esp == TOP - 4);
    CHECK(ebp == 0xCAFEF00Du);

    /* pushad; popad: popad skips the saved-ESP slot, so ESP returns to the
     * value before pushad even if the slot was overwritten. */
    esp = TOP; eax = 1; ecx = 2; edx = 3; ebx = 4; ebp = 5; esi = 6; edi = 7;
    { uint32_t _pa_esp = esp; PUSH32(esp, eax); PUSH32(esp, ecx);
      PUSH32(esp, edx); PUSH32(esp, ebx); PUSH32(esp, _pa_esp);
      PUSH32(esp, ebp); PUSH32(esp, esi); PUSH32(esp, edi); }
    MEM32(esp + 12) = 0x11111111u;
    eax = ecx = edx = ebx = ebp = esi = edi = 0;
    POP32(esp, edi); POP32(esp, esi); POP32(esp, ebp); esp += 4;
    POP32(esp, ebx); POP32(esp, edx); POP32(esp, ecx); POP32(esp, eax);
    CHECK(esp == TOP);
    CHECK(eax == 1 && ecx == 2 && edx == 3 && ebx == 4 && ebp == 5 && esi == 6 && edi == 7);

    if (failures) {
        fprintf(stderr, "%u check(s) failed\n", failures);
        return 1;
    }
    puts("PASS: push/pop esp, round-to-int idiom, ordinary pop, push/pop [esp+4], leave, pushad/popad");
    return 0;
}
