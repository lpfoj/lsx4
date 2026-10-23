// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// ELF weak declarations and Mach-O weak imports have different attributes.
// An unavailable optional backend must resolve to nullptr on either platform.
#if defined(__APPLE__)
#define LSX4_OPTIONAL_HOST_IMPORT __attribute__((weak_import))
#else
#define LSX4_OPTIONAL_HOST_IMPORT __attribute__((weak))
#endif
