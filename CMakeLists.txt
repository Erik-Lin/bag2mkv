cmake_minimum_required(VERSION 3.16)
project(bag2mkv VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(K4A_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/azurekinectsdk")

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

add_library(bag2mkv_core STATIC
    src/bag.cpp
    src/scan.cpp
    src/lz4_util.cpp
    src/util.cpp
    src/k4a_writer.cpp
    src/jpeg_decoder.cpp
    src/manifest.cpp
    src/converter.cpp
)
target_include_directories(bag2mkv_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${K4A_ROOT}/include"
)
target_compile_definitions(bag2mkv_core PUBLIC BAG2MKV_VERSION="${PROJECT_VERSION}")
target_link_libraries(bag2mkv_core PUBLIC
    lz4
    "${K4A_ROOT}/lib/k4arecord.lib"
    "${K4A_ROOT}/lib/k4a.lib"
    bcrypt
    windowscodecs
    ole32
)
if(MSVC)
    target_compile_options(bag2mkv_core PRIVATE /W4 /utf-8)
endif()

add_executable(bag2mkv src/main.cpp)
target_link_libraries(bag2mkv PRIVATE bag2mkv_core)
if(MSVC)
    target_compile_options(bag2mkv PRIVATE /W4 /utf-8)
endif()

add_executable(bag2mkv-gui WIN32 src/gui_win32.cpp)
target_link_libraries(bag2mkv-gui PRIVATE bag2mkv_core comctl32 comdlg32 shell32)
set_target_properties(bag2mkv-gui PROPERTIES WIN32_EXECUTABLE TRUE)
if(MSVC)
    target_compile_options(bag2mkv-gui PRIVATE /W4 /utf-8)
    target_compile_definitions(bag2mkv-gui PRIVATE UNICODE _UNICODE)
endif()

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
copy_k4a_runtime(bag2mkv-gui)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME bag2mkv_self_test COMMAND $<TARGET_FILE:bag2mkv> --self-test)
    add_test(NAME bag2mkv_help COMMAND $<TARGET_FILE:bag2mkv> --help)
    add_test(NAME bag2mkv_batch_help COMMAND $<TARGET_FILE:bag2mkv> batch --help)
endif()
