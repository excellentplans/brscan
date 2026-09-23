#ifndef BRSCAN5_DBG_H
#define BRSCAN5_DBG_H

#include <stdarg.h>

/* Copyright 2025 excellentplans. SPDX-License-Identifier: GPL-2.0-or-later
 * brscan5-layer logging: every log line goes through DBG(level, ...) on
 * the SANE debug channel (SANE_DEBUG_BROTHER, "[brother] " stderr
 * prefix). The brscan5 TUs must not include <sane/sanei_debug.h>
 * directly: with NDEBUG (Release builds) it compiles DBG down to a
 * no-op and drops the debug-level variable entirely, and without NDEBUG
 * every TU including it would define its own `sanei_debug_brother` copy
 * (multiple definitions under -fno-common). Instead DBG() routes through
 * the shared sanei_debug_msg()/sanei_init_debug() (sanei_init_debug.c,
 * part of the backend .so) with a brscan5-owned level cell initialized
 * once from SANE_DEBUG_BROTHER — same env var and level convention as
 * the legacy DBG() in brother.c (output when level <= debug level):
 *   DBG(1) errors/faults, DBG(3) state transitions, DBG(5) traces. */

void brscan5_dbg(int level, const char *fmt, ...);

/* shared SANE debug plumbing (sanei_init_debug.c) */
void sanei_debug_msg(int level, int max_level, const char *be,
                     const char *fmt, va_list ap);
void sanei_init_debug(const char *backend, int *debug_level_var);

#define DBG(level, ...) brscan5_dbg((level), __VA_ARGS__)

#endif /* BRSCAN5_DBG_H */
