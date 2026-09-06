"""Compile the actual mixer and DirectSound buffer code, without audio hardware.

The APU core also contains the hardware emulation and host output backend.
Take its mixer declarations and implementation verbatim; only the APU wakeup
target is stubbed (NULL). Requires GCC on PATH, including MinGW on Windows.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
core = (root / "src/apu/apu_core.c").read_text(encoding="utf-8")
declarations = core[core.index("static void mixer_init(void);"):
                    core.index("struct McpxApuDebug g_dbg;")]
implementation = core[core.index("static void mixer_init(void)\n{"):]
source = """
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "platform/xbox_winnt.h"
#include "apu/apu.h"
struct MCPXAPUState { int lock, cond; bool pause_requested; };
MCPXAPUState *g_state = NULL;
int g_audio_muted = 0;
static void qemu_mutex_lock(int *p) { (void)p; }
static void qemu_mutex_unlock(int *p) { (void)p; }
static void qemu_cond_signal(int *p) { (void)p; }
""" + declarations + implementation
source += (root / "tests/audio_mixer/test_main.c").read_text(encoding="utf-8")
with tempfile.TemporaryDirectory(prefix="audio-mixer-") as directory:
    exe = str(pathlib.Path(directory) / "test.exe")
    command = ["gcc", "-std=c11", "-Isrc", "-x", "c", "-",
               "src/audio/dsound_device.c", "-o", exe]
    if sys.platform != "win32":
        command += ["src/platform/win32_compat.c", "-lm", "-lpthread"]
    subprocess.run(command, cwd=root, input=source, text=True, check=True)
    subprocess.run([exe], check=True, timeout=10)
