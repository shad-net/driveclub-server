# SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.19)
project(driveclubsrv VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Network)

# ---- protobuf ------------------------------------------------------------------------------
# protobuf v21.12 is a git submodule in externals/ (built in-tree, like shadNet's externals); it
# is the last release that does not need abseil. Clone with --recursive, or run
# `git submodule update --init --recursive`. The game embeds protobuf 2.4.1, but the wire format is
# unchanged since then; the schema sent to the game is reduced to 2.4.1 descriptor fields in
# egs.cpp.
if(MSVC)
    # Qt uses the DLL runtime (/MD, /MDd); protobuf must match.
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
    add_compile_options(/utf-8)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/CMakeLists.txt")
    message(FATAL_ERROR "externals/protobuf is empty: run `git submodule update --init --recursive`")
endif()
add_subdirectory(externals)

set(EGS_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/proto/egs.proto")
set(EGS_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${EGS_PROTO_OUT}")
add_custom_command(
    OUTPUT "${EGS_PROTO_OUT}/egs.pb.cc" "${EGS_PROTO_OUT}/egs.pb.h"
    COMMAND $<TARGET_FILE:protoc>
            "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto"
            "--cpp_out=${EGS_PROTO_OUT}"
            "${EGS_PROTO}"
    DEPENDS "${EGS_PROTO}" protoc
    COMMENT "Generating EGS protobuf sources"
    VERBATIM)

# Generated code gets its own target so the strict warning flags below don't apply to it.
add_library(egs_proto STATIC "${EGS_PROTO_OUT}/egs.pb.cc" "${EGS_PROTO_OUT}/egs.pb.h")
target_include_directories(egs_proto PUBLIC
    "${EGS_PROTO_OUT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src")
target_link_libraries(egs_proto PUBLIC libprotobuf)

add_executable(driveclubsrv
    src/config.cpp src/config.h
    src/egs.cpp src/egs.h
    src/file_log.cpp src/file_log.h
    src/handler.cpp src/handler.h
    src/http_server.cpp src/http_server.h
    src/main.cpp
    src/otg.cpp src/otg.h
    src/proto_dump.cpp src/proto_dump.h
    src/trace.cpp src/trace.h
)
target_link_libraries(driveclubsrv PRIVATE Qt6::Core Qt6::Network egs_proto)
# moc only for the server itself, not for the protobuf targets in externals/.
set_target_properties(driveclubsrv PROPERTIES AUTOMOC ON)

if(MSVC)
    target_compile_options(driveclubsrv PRIVATE /W4 /permissive-)
else()
    target_compile_options(driveclubsrv PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    option(GAME_SERVER_DEPLOY_QT "Run windeployqt after building" ON)
    if(GAME_SERVER_DEPLOY_QT)
        find_program(WINDEPLOYQT_EXECUTABLE windeployqt
            HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" "${Qt6_DIR}/../../../bin")
        if(WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET driveclubsrv POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                        "$<TARGET_FILE:driveclubsrv>"
                COMMENT "Deploying Qt next to driveclubsrv.exe"
                VERBATIM)
        else()
            message(WARNING "windeployqt not found in Qt's bin folder: Qt DLLs will not be copied")
        endif()
    endif()
endif()

install(TARGETS driveclubsrv RUNTIME DESTINATION bin)
