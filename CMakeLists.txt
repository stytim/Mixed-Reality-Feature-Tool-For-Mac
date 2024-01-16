cmake_minimum_required(VERSION 3.10)

# Set your project name
project(MRTK_Component_Fetcher)

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
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)

# Add the executable
add_executable(mrfeature_tool main.cpp)

# Link the libraries to your executable
target_link_libraries(mrfeature_tool PRIVATE CURL::libcurl ${LibArchive_LIBRARIES})

target_include_directories(mrfeature_tool PRIVATE ${LibArchive_INCLUDE_DIRS})
