/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_LOG_H
#define KR_LOG_H

#include <stdbool.h>

/* Messages go to stderr prefixed with the program name. Debug messages
 * only show with kr_log_set_verbose(true) (-v). */
void kr_log_init(const char *program);
void kr_log_set_verbose(bool on);
bool kr_log_verbose(void);

void kr_log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kr_log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kr_log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kr_log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
