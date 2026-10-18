/*
 * enhance_input.h - the [input] and [input.bindings] keys (the port-0
 * keyboard and mouse) read from the configuration files into src/input.
 */
#ifndef ENHANCE_INPUT_H
#define ENHANCE_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called by xbox_enhance_init after enhance_cfg_init: binds input.keyboard
 * and input.mouse to RECOMP_KEYBOARD and RECOMP_MOUSE, reads the knobs and
 * the bindings, applies them through src/input, and logs one
 * "[INPUT] config:" line. Errors are [INPUT] lines and never fatal; returns
 * 0. A second call re-reads (tests). */
int enhance_input_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_INPUT_H */
