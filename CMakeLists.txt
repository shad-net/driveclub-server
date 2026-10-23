# SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.19)
project(driveclubsrv VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Network)

add_executable(driveclubsrv
    src/config.cpp src/config.h
    src/file_log.cpp src/file_log.h
    src/handler.cpp src/handler.h
    src/http_server.cpp src/http_server.h
    src/main.cpp
    src/trace.cpp src/trace.h
)
target_link_libraries(driveclubsrv PRIVATE Qt6::Core Qt6::Network)

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
