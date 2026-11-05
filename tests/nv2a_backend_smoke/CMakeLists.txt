# nv2a_backend_smoke - a GPU render backend of the pushbuffer executor
# against the CPU rasteriser, on a POSIX host. Feeds nv2a_pb_exec_method() a
# hand-written method stream (surface, clear, quads, flip) once per backend
# and compares the presented frames within the golden limits. The backend is
# RECOMP_SMOKE_BACKEND (default metal); exit 77 (skipped) when the host has
# none. RECOMP_SMOKE_DUMP=<prefix> writes the frames as BMPs.
#   cmake --build <build> --target nv2a_backend_smoke

add_executable(nv2a_backend_smoke smoke.c)
target_link_libraries(nv2a_backend_smoke PRIVATE xboxrecomp)
target_include_directories(nv2a_backend_smoke PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME nv2a_backend_smoke COMMAND nv2a_backend_smoke)
add_test(NAME nv2a_backend_smoke_guard COMMAND nv2a_backend_smoke guard)
add_test(NAME nv2a_backend_smoke_noalias COMMAND nv2a_backend_smoke noalias)
set_tests_properties(nv2a_backend_smoke nv2a_backend_smoke_guard nv2a_backend_smoke_noalias
    PROPERTIES SKIP_RETURN_CODE 77)
