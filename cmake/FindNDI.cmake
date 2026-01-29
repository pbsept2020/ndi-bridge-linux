# FindNDI.cmake - Locate NDI SDK for Linux
#
# This module defines:
#   NDI_FOUND        - True if NDI was found
#   NDI_INCLUDE_DIRS - NDI include directories
#   NDI_LIBRARIES    - NDI libraries to link
#   NDI_LIBRARY_DIRS - NDI library directories
#   NDI_VERSION      - NDI SDK version (if available)

# Search paths for NDI SDK
# On macOS, prefer the official Apple SDK (v6.3+) over /usr/local (older)
if(APPLE)
    set(NDI_SEARCH_PATHS
        # macOS: Official NDI SDK for Apple (latest, v6.3+)
        "/Library/NDI SDK for Apple"
        # Environment variable
        "$ENV{NDI_SDK_DIR}"
        # Fallback system paths
        /usr/local
        /usr
    )
else()
    set(NDI_SEARCH_PATHS
        # Standard system paths (Linux)
        /usr
        /usr/local
        # User home directory installation (Linux)
        "$ENV{HOME}/NDI SDK for Linux"
        "$ENV{HOME}/ndi-sdk"
        # Environment variable
        "$ENV{NDI_SDK_DIR}"
        # Common installation paths (Linux)
        /opt/ndi-sdk
        /opt/NDI
    )
endif()

# Find include directory
# Use NO_DEFAULT_PATH on macOS to avoid picking up old /usr/local headers
find_path(NDI_INCLUDE_DIR
    NAMES Processing.NDI.Lib.h
    PATHS ${NDI_SEARCH_PATHS}
    PATH_SUFFIXES
        include
        Include
        "include/ndi"
        "include"
    ${APPLE_NO_DEFAULT}
)

# Find library - platform specific names
if(APPLE)
    set(NDI_LIB_NAMES ndi "NDI")
    # On macOS, search our preferred paths first without cmake defaults
    find_library(NDI_LIBRARY
        NAMES ${NDI_LIB_NAMES}
        PATHS ${NDI_SEARCH_PATHS}
        PATH_SUFFIXES
            lib
            "lib/macOS"
        NO_DEFAULT_PATH
    )
else()
    set(NDI_LIB_NAMES ndi)
    find_library(NDI_LIBRARY
        NAMES ${NDI_LIB_NAMES}
        PATHS ${NDI_SEARCH_PATHS}
        PATH_SUFFIXES
            lib
            lib/x86_64-linux-gnu
            lib64
            "lib/x86_64-linux-gnu"
    )
endif()

# Extract version from header if found
if(NDI_INCLUDE_DIR AND EXISTS "${NDI_INCLUDE_DIR}/Processing.NDI.Lib.h")
    file(STRINGS "${NDI_INCLUDE_DIR}/Processing.NDI.Lib.h" NDI_VERSION_LINE
         REGEX "#define.*NDI_LIB_VERSION")
    if(NDI_VERSION_LINE)
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" NDI_VERSION "${NDI_VERSION_LINE}")
    endif()
endif()

# Get library directory
if(NDI_LIBRARY)
    get_filename_component(NDI_LIBRARY_DIR "${NDI_LIBRARY}" DIRECTORY)
endif()

# Handle standard find_package arguments
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NDI
    REQUIRED_VARS NDI_LIBRARY NDI_INCLUDE_DIR
    VERSION_VAR NDI_VERSION
)

# Set output variables
if(NDI_FOUND)
    set(NDI_INCLUDE_DIRS ${NDI_INCLUDE_DIR})
    set(NDI_LIBRARIES ${NDI_LIBRARY})
    set(NDI_LIBRARY_DIRS ${NDI_LIBRARY_DIR})

    # Create imported target
    if(NOT TARGET NDI::NDI)
        add_library(NDI::NDI SHARED IMPORTED)
        set_target_properties(NDI::NDI PROPERTIES
            IMPORTED_LOCATION "${NDI_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NDI_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(NDI_INCLUDE_DIR NDI_LIBRARY)
