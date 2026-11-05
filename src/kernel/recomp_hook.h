/**
 * recomp_hook.h - hook slots: where a mod can run code around a guest function.
 *
 * A mod asks for a hook on a guest VA with the mod API's hook() (mod.h). The
 * recompiler cannot add a call to every function on the chance that a mod
 * wants it, so the game declares the VAs it wants hookable (the CLI's
 * [mods] hook_functions list in game.toml) and the generated code gives each one a
 * slot: a small static record, and a dispatch around the function's body
 * that costs one load and a predicted branch while no mod has hooked it.
 *
 * This header is the whole interface between the generated code and the
 * runtime. The generated code:
 *
 *   static struct recomp_hook_slot s_hook_slots[] = {
 *       { 0x0003B180u, 0x14, 0, 0, NULL },          // ret 0x14
 *       { 0x00041A20u, 0,    RECOMP_HOOK_SLOT_ST0, 0, NULL }, // returns in st0
 *   };
 *   RECOMP_HOOK_SLOTS(s_hook_slots, 2)
 *
 *   static void sub_0003B180_body(void) { ...the lifted body... }
 *   void sub_0003B180(void) { RECOMP_HOOK_CALL(&s_hook_slots[0], sub_0003B180_body); }
 *
 * RECOMP_HOOK_SLOTS registers the table from a static constructor, so the
 * runtime knows every slot before any mod's init. A game with no hooks
 * emits neither, and its generated code is the same as before.
 *
 * The runtime owns `chain`: it stays NULL until a mod's hook on that VA
 * survives init, and is published once, after every init, on the guest's
 * thread before the game runs; shutdown sets it back to NULL. The
 * generated code only reads it.
 *
 * What a mod's hook sees (struct xbox_mod_call) and the runtime's calls are
 * in recomp_hook_mod.h, which the generated code never includes: its
 * register names (eax, esp, ...) are macros there (recomp_types.h).
 *
 * On MSVC (cl, not clang) a table's name becomes a global symbol, so each
 * table needs a name unique in the program (s_hook_slots_<file>, say).
 */
#ifndef RECOMP_HOOK_H
#define RECOMP_HOOK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The generated code's side ------------------------------------------- */

/* The function returns a float in st0: a skip pushes c->st0, and the post
 * hooks get st0 with st0_valid set. */
#define RECOMP_HOOK_SLOT_ST0 0x01u

struct recomp_hook_chain; /* the runtime's; opaque here */

struct recomp_hook_slot {
    uint32_t va;      /* the guest function's entry */
    uint16_t ret_pop; /* the imm16 of its `ret n`: bytes a skip pops after the return address */
    uint8_t flags;    /* RECOMP_HOOK_SLOT_* */
    uint8_t reserved; /* 0 */
    const struct recomp_hook_chain *volatile chain; /* runtime-owned; NULL = not hooked */
};

/* Called by RECOMP_HOOK_SLOTS before main. `slots` must stay valid for the
 * life of the process (a static array). Several tables may be registered,
 * one per generated file; a VA in two tables keeps the first and logs. */
void recomp_hook_slots_register(struct recomp_hook_slot *slots, size_t n);

/* Run a hooked call: the pre hooks, the body unless skipped, the post hooks.
 * Only called through RECOMP_HOOK_CALL when slot->chain is set. */
void recomp_hook_run(struct recomp_hook_slot *slot, void (*body)(void));

#if defined(__GNUC__) || defined(__clang__)
#  define RECOMP_HOOK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define RECOMP_HOOK_UNLIKELY(x) (x)
#endif

/* The dispatch a hooked function's entry becomes. */
#define RECOMP_HOOK_CALL(slot, body) do { \
    if (RECOMP_HOOK_UNLIKELY((slot)->chain != NULL)) recomp_hook_run((slot), (body)); \
    else (body)(); \
} while (0)

/* Register a slot table from a static constructor. Use once per table, at
 * file scope, after its definition. */
#if defined(_MSC_VER) && !defined(__clang__)
/* The pointer is a global and /include keeps it: under /O2 /GL the linker
 * drops an unreferenced static one, and the table would never register. */
#  if defined(_M_IX86)
#    define RECOMP_HOOK_SYM_PREFIX_ "_"
#  else
#    define RECOMP_HOOK_SYM_PREFIX_ ""
#  endif
#  pragma section(".CRT$XCU", read)
#  define RECOMP_HOOK_SLOTS(table, n) \
    static void recomp_hook_reg_##table(void) { recomp_hook_slots_register((table), (n)); } \
    __declspec(allocate(".CRT$XCU")) void (*recomp_hook_reg_ptr_##table)(void) = \
        recomp_hook_reg_##table; \
    __pragma(comment(linker, "/include:" RECOMP_HOOK_SYM_PREFIX_ "recomp_hook_reg_ptr_" #table))
#else
#  define RECOMP_HOOK_SLOTS(table, n) \
    __attribute__((constructor)) static void recomp_hook_reg_##table(void) { \
        recomp_hook_slots_register((table), (n)); \
    }
#endif

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_HOOK_H */
