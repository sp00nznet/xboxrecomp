add_executable(tool ../main.c)
if(CALL_TOOL)
    xboxrecomp_present_shaders(tool)
endif()
