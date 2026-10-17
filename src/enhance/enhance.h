/*
 * enhance.h - the enhancements layer's entry point (opt-in,
 * XBOXRECOMP_ENHANCE=ON).
 *
 * A title calls xbox_enhance_init once at startup, after recomp_env_init and
 * before the guest (and so any backend) starts. It loads config.toml over
 * config.default.toml (enhance_cfg.h), reads the toolkit's own keys and
 * hands them to the backends and the presenter through nv2a_host_opts_set
 * (nv2a_backend_common.h). Those never link this library: without it the
 * options stay all zero, which is stock.
 *
 * Keys read here (docs/runtime/config.md):
 *   render.scale        1..4, internal resolution factor (Metal, D3D11)
 *   present.filter      nearest | linear | integer
 *   present.fullscreen  borderless desktop fullscreen at start
 *   present.pacing      sleep (default) | spin (stock, the console-original
 *                       timing): how lowered guest spin waits wait
 *                       (kernel_pacing.h)
 *   display.aspect      only 4:3 so far; anything else is reported
 *
 * Logs one line with the values in force, e.g.
 *   [ENHANCE] render.scale=2 present.filter=linear present.fullscreen=0 present.pacing=sleep (display.aspect=4:3)
 * which scripts/golden.py reads to refuse a non-stock golden run.
 *
 * It does not report unused keys: the title reads its own keys after this
 * and then calls enhance_cfg_report_unused() (enhance_cfg.h), so a title
 * key in config.toml is not reported as unused.
 */
#ifndef XBOX_ENHANCE_H
#define XBOX_ENHANCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* `root` is the user data root, which holds config/config.toml (a
 * developer run passes ".", the repository; a bundle's launcher names the
 * file with RECOMP_CONFIG), `title` an optional per-title subdirectory
 * (NULL for one title per executable). config.default.toml and a relative
 * textures.pack are found from the executable's directory. Returns 0, or
 * -1 if a config file failed to parse (it is skipped whole; the other
 * files and the environment apply). */
int xbox_enhance_init(const char *root, const char *title);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_ENHANCE_H */
