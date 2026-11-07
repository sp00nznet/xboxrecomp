/*
 * demo_mod.h - the template mod's one entry point (demo_mod.c,
 * docs/runtime/mod-howto.md).
 *
 * The title's table is `const struct xbox_mod *const`, but this header stays
 * free of toolkit headers: main.c includes it in a build without the
 * enhancements layer too, where the accessor returns NULL.
 */
#ifndef DEMO_MOD_H
#define DEMO_MOD_H

/* The mod for xbox_mod_register_table (a const struct xbox_mod *), or NULL
 * when the toolkit in use has no mod API this mod can run on. */
const void *demo_mod(void);

#endif /* DEMO_MOD_H */
