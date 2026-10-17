# template_main_defaults: the template carries the lines every XDK D3D title
# needed by hand (BLiNX 2, Burnout 3, Spartan): RECOMP_VBLANK and
# RECOMP_TITLE_KEVENTS defaulted on, each only when the environment leaves it
# unset, and the commented fence-mirror example. xbr new replaces that example
# with the live line when it finds D3D's fence wait in the dump, matching the
# text exactly, so a reworded example would silently stop the CLI's patch.
# And the window title, one call per entry point.
#   cmake -DMAIN=<template main.c> -P defaults.cmake
file(READ "${MAIN}" text)

foreach(key VBLANK TITLE_KEVENTS)
    set(want "if \\(!recomp_env\\(RENV_${key}\\)\\)[ \t\r\n]+recomp_env_set\\(RENV_${key}, \"1\"\\);")
    if(NOT text MATCHES "${want}")
        message(FATAL_ERROR "${MAIN}: no RENV_${key} default guarded by !recomp_env(RENV_${key})")
    endif()
endforeach()

string(FIND "${text}" "    /* xbox_Nv2aMirrorFence(0x00000000, 0x2C, 0x30); */\n" at)
if(at EQUAL -1)
    message(FATAL_ERROR "${MAIN}: no fence mirror example line (xbr new patches that exact text)")
endif()
# The window title line: xbr new replaces YOUR_GAME_NAME everywhere in the
# file, so both entry points name the game's window.
string(REGEX MATCHALL "xbox_HostWindowSetTitle\\(\"YOUR_GAME_NAME\"\\)" titles "${text}")
list(LENGTH titles n)
if(NOT n EQUAL 2)
    message(FATAL_ERROR "${MAIN}: ${n} xbox_HostWindowSetTitle(\"YOUR_GAME_NAME\") calls, want 2 (WinMain and main)")
endif()
message(STATUS "template_main defaults: ok")
