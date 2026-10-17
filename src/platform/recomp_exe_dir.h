/*
 * recomp_exe_dir.h - the directory that holds the running executable.
 *
 * Files that belong to an installed game (config.default.toml, a texture
 * pack) sit beside the executable, not in the working directory, so a launcher
 * or a shortcut that starts the game from elsewhere still finds it.
 */
#ifndef RECOMP_EXE_DIR_H
#define RECOMP_EXE_DIR_H

#ifdef __cplusplus
extern "C" {
#endif

/* The executable's directory, without a trailing separator ("." when it
 * cannot be found, i.e. the working directory). Resolved once and cached;
 * call it first from one thread (startup). */
const char *recomp_exe_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_EXE_DIR_H */
