# SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(lsx4_ios_jit LANGUAGES C CXX OBJCXX ASM)
if(NOT APPLE)
    message(FATAL_ERROR "This branch targets Apple platforms")
endif()
add_compile_definitions(LSX4_JIT_ONLY=1)
add_subdirectory(LSX4iOS/RuntimeCore)
