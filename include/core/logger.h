// -*- coding: utf-8 -*-
// core/logger.h — Simple timestamped logger (stderr)
#pragma once

#include <cstdio>
#include <ctime>
#include <cstdarg>

enum LogLevel { LOG_DEBUG, LOG_WARNING, LOG_ERROR, LOG_CRITICAL };

inline void log_msg(LogLevel level, const char* fmt, ...) {
    const char* prefix[] = {"[DEBUG]", "[WARN]", "[ERROR]", "[CRIT]"};
    if (level < LOG_WARNING) return;  // Only warn+ in production

    auto t = std::time(nullptr);
    char tbuf[32];
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

    fprintf(stderr, "%s %s ", tbuf, prefix[level]);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
