cmake_minimum_required(VERSION 3.10)

# Set your project name
project(MRTKFeatureTool-CPP)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    # Homebrew ships libarchive keg only, include dirs have to be set manually
    execute_process(
        COMMAND brew --prefix libarchive
        OUTPUT_VARIABLE LIBARCHIVE_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    set(LibArchive_INCLUDE_DIR "${LIBARCHIVE_PREFIX}/include")
endif()
# Find the required libraries
find_package(wxWidgets REQUIRED)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)

include(${wxWidgets_USE_FILE})
include_directories(${LibArchive_INCLUDE_DIRS})

# This creates a library named 'mrtk_core' that both executables can use.
add_library(mrtk_core core_logic.cpp core_logic.h)
target_include_directories(mrtk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mrtk_core
    PUBLIC
    CURL::libcurl
    ${LibArchive_LIBRARIES}
)

# --- CLI Executable ---
add_executable(mrtk_cli cli_main.cpp)
target_link_libraries(mrtk_cli PRIVATE mrtk_core)

# # # --- GUI Executable ---
add_executable(mrtk_gui gui_main.cpp)
target_include_directories(mrtk_gui PRIVATE ${wxWidgets_INCLUDE_DIRS})
target_link_libraries(mrtk_gui
    PRIVATE
    mrtk_core
    ${wxWidgets_LIBRARIES}
)


