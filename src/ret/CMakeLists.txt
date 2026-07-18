# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026-2026, The OpenROAD Authors

include("openroad")
swig_lib(
    NAME ret
    NAMESPACE ret
    I_FILE src/Retimer.i
    SCRIPTS src/Retimer.tcl
    SWIG_INCLUDES ${ODB_HOME}/src/swig/common
                  ${ODB_HOME}/src/swig/tcl
                  ${ODB_HOME}/include    
)

add_library(ret_lib
    src/Retimer.cpp
)

# link external libraries
# add libraries here
target_link_libraries(ret_lib
    PUBLIC
        odb
    PRIVATE
        utl_lib
)

target_include_directories(ret_lib
    PUBLIC
        include
    PRIVATE
        src

)

# OpenROAD depends on ret
target_sources(ret
    PRIVATE
        src/MakeRetimer.cpp
)

target_include_directories(ret
    PUBLIC
        include
    PRIVATE
        src
)

target_link_libraries(ret
    PRIVATE
        ${TCL_LIBRARY}
        ret_lib
)

messages(TARGET ret)