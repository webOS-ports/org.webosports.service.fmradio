# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.10)
project(org.webosports.service.fmradio C)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC4)
webos_component(1 0 0)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(LS2 REQUIRED luna-service2)
pkg_check_modules(PBNJSON_C REQUIRED pbnjson_c)
pkg_check_modules(PULSE REQUIRED libpulse-mainloop-glib)

include_directories(${GLIB2_INCLUDE_DIRS} ${LS2_INCLUDE_DIRS}
                    ${PBNJSON_C_INCLUDE_DIRS} ${PULSE_INCLUDE_DIRS})

webos_add_compiler_flags(ALL -Wall -Wextra)

add_executable(fmradiod src/main.c src/fm_band.c src/fm_tuner.c src/fm_mtk.c src/fm_iris.c
                        src/fm_rds.c src/fm_audio.c)
target_link_libraries(fmradiod ${GLIB2_LDFLAGS} ${LS2_LDFLAGS}
                               ${PBNJSON_C_LDFLAGS} ${PULSE_LDFLAGS})

webos_build_daemon(NAME fmradiod LAUNCH files/launch)
webos_build_system_bus_files()
