#
# Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
#
# This file is part of System Informer.
#

set(HEADERS
    "resource.h"
    "wsltools.h"
)
source_group("Header Files" FILES ${HEADERS})

set(RESOURCES
    "version.rc"
    "WslTools.rc"
)
source_group("Resource Files" FILES ${RESOURCES})

set(SOURCES
    "main.c"
    "wslc.c"
    "wslproc.c"
    "wslprv.c"
    "wslsys.c"
    "wsltab.c"
    "wslutil.c"
)
source_group("Source Files" FILES ${SOURCES})

set(ALL_FILES
    ${HEADERS}
    ${RESOURCES}
    ${SOURCES}
)

si_add_plugin(WslTools ${ALL_FILES})

target_link_libraries(WslTools PRIVATE
    user32
)
