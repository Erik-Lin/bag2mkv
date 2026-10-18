cmake_minimum_required(VERSION 3.16)
project(bag2mkv VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    set(K4A_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/azurekinectsdk" CACHE PATH "Orbbec K4A Wrapper SDK root")
    option(BUILD_GUI "Build the Windows GUI (bag2mkv-gui)" ON)
else()
    set(K4A_ROOT "" CACHE PATH "Optional K4A SDK root (with include/ and lib/)")
    option(BUILD_GUI "Build the Windows GUI (bag2mkv-gui)" OFF)
endif()

add_library(lz4 STATIC
    third_party/lz4/lz4.c
    third_party/lz4/lz4frame.c
    third_party/lz4/lz4hc.c
    third_party/lz4/xxhash.c
)
target_include_directories(lz4 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4")
if(MSVC)
    target_compile_options(lz4 PRIVATE /W0)
endif()

add_library(bz2 STATIC
    third_party/bzip2/bzlib.c
    third_party/bzip2/blocksort.c
    third_party/bzip2/huffman.c
    third_party/bzip2/crctable.c
    third_party/bzip2/randtable.c
    third_party/bzip2/compress.c
    third_party/bzip2/decompress.c
)
target_include_directories(bz2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bzip2")
if(MSVC)
    target_compile_options(bz2 PRIVATE /W0)
endif()

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
    target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION)
endif()

add_library(bag2mkv_core STATIC
    src/bag.cpp
    src/bag_writer.cpp
    src/json.cpp
    src/scan.cpp
    src/lz4_util.cpp
    src/bz2_util.cpp
    src/util.cpp
    src/k4a_writer.cpp
    src/jpeg_decoder.cpp
    src/manifest.cpp
    src/converter.cpp
    src/mkv2bag.cpp
    src/ros1_export.cpp
    src/ros1_input.cpp
    src/ros2_export.cpp
    src/ros2_input.cpp
)
target_include_directories(bag2mkv_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
)
target_compile_definitions(bag2mkv_core PUBLIC BAG2MKV_VERSION="${PROJECT_VERSION}")
target_link_libraries(bag2mkv_core PUBLIC lz4 bz2 sqlite3)

if(WIN32)
    target_include_directories(bag2mkv_core PUBLIC "${K4A_ROOT}/include")
    target_link_libraries(bag2mkv_core PUBLIC
        "${K4A_ROOT}/lib/k4arecord.lib"
        "${K4A_ROOT}/lib/k4a.lib"
        bcrypt
        windowscodecs
        ole32
    )
else()
    find_path(K4A_INCLUDE_DIR
        NAMES k4a/k4a.h
        HINTS "${K4A_ROOT}/include" /usr/include /usr/local/include
    )
    find_library(K4A_LIBRARY
        NAMES k4a
        HINTS "${K4A_ROOT}/lib" /usr/lib/x86_64-linux-gnu /usr/local/lib
    )
    find_library(K4A_RECORD_LIBRARY
        NAMES k4arecord
        HINTS "${K4A_ROOT}/lib" /usr/lib/x86_64-linux-gnu /usr/local/lib
    )
    if(NOT K4A_INCLUDE_DIR OR NOT K4A_LIBRARY OR NOT K4A_RECORD_LIBRARY)
        message(FATAL_ERROR
            "Linux K4A SDK not found. Set -DK4A_ROOT=/path/to/OrbbecSDK-K4A-Wrapper "
            "with include/ and lib/ directories.")
    endif()
    find_package(Threads REQUIRED)
    target_include_directories(bag2mkv_core PUBLIC "${K4A_INCLUDE_DIR}")
    target_link_libraries(bag2mkv_core PUBLIC
        "${K4A_RECORD_LIBRARY}"
        "${K4A_LIBRARY}"
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
endif()

if(MSVC)
    target_compile_options(bag2mkv_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(bag2mkv_core PRIVATE -Wall -Wextra)
endif()

add_executable(bag2mkv src/main.cpp)
target_link_libraries(bag2mkv PRIVATE bag2mkv_core)
if(NOT WIN32)
    set_target_properties(bag2mkv PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()
if(MSVC)
    target_compile_options(bag2mkv PRIVATE /W4 /utf-8)
else()
    target_compile_options(bag2mkv PRIVATE -Wall -Wextra)
endif()

if(WIN32 AND BUILD_GUI)
    add_executable(bag2mkv-gui WIN32 src/gui_win32.cpp src/gui_backend.cpp)
    target_link_libraries(bag2mkv-gui PRIVATE bag2mkv_core comctl32 comdlg32 shell32 dwmapi)
    set_target_properties(bag2mkv-gui PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_compile_options(bag2mkv-gui PRIVATE /W4 /utf-8)
        target_compile_definitions(bag2mkv-gui PRIVATE UNICODE _UNICODE)
    endif()
endif()

if(WIN32)
    function(copy_k4a_runtime target)
        file(GLOB K4A_RUNTIME_DLLS "${K4A_ROOT}/bin/*.dll")
        if(K4A_RUNTIME_DLLS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${K4A_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copying K4A runtime DLLs"
            )
        endif()
    endfunction()

    copy_k4a_runtime(bag2mkv)
    if(TARGET bag2mkv-gui)
        copy_k4a_runtime(bag2mkv-gui)
    endif()
endif()

install(TARGETS bag2mkv RUNTIME DESTINATION bin)
if(WIN32 AND BUILD_GUI)
    install(TARGETS bag2mkv-gui RUNTIME DESTINATION bin)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME bag2mkv_self_test COMMAND $<TARGET_FILE:bag2mkv> --self-test)
    add_test(NAME bag2mkv_help COMMAND $<TARGET_FILE:bag2mkv> --help)
    add_test(NAME bag2mkv_batch_help COMMAND $<TARGET_FILE:bag2mkv> batch --help)
    add_test(NAME bag2mkv_mkv2bag_help COMMAND $<TARGET_FILE:bag2mkv> mkv2bag --help)
    add_test(NAME bag2mkv_ros1_help COMMAND $<TARGET_FILE:bag2mkv> ros1 --help)
    add_test(NAME bag2mkv_ros1mkv_help COMMAND $<TARGET_FILE:bag2mkv> ros1mkv --help)
    add_test(NAME bag2mkv_ros2_help COMMAND $<TARGET_FILE:bag2mkv> ros2 --help)
    add_test(NAME bag2mkv_ros2mkv_help COMMAND $<TARGET_FILE:bag2mkv> ros2mkv --help)
    add_test(NAME bag2mkv_mkv2ros1_help COMMAND $<TARGET_FILE:bag2mkv> mkv2ros1 --help)
    add_test(NAME bag2mkv_mkv2ros2_help COMMAND $<TARGET_FILE:bag2mkv> mkv2ros2 --help)
endif()
