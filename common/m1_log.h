// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once

/*
 * Writes <userfs>/Programs/m1.log, at RFX_USERFS_PROGRAMS_PATH. Only an
 * M1_DEBUG build writes; otherwise the calls compile to nothing. Nothing is
 * logged from the audio path: a flash write in a block crackles and wears the
 * memory.
 */

#include <cstdarg>
#include <cstdio>
#include <cstdint>

#if defined(__linux__)
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef RFX_USERFS_PROGRAMS_PATH
#define RFX_USERFS_PROGRAMS_PATH /var/lib/drumlogued/userfs/Programs
#endif

#define RFX_STRINGIFY_IMPL(value) #value
#define RFX_STRINGIFY(value) RFX_STRINGIFY_IMPL(value)

#define M1_LOG_PATH RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH) "/m1.log"

namespace m1unit {

inline FILE* logFile() {
    static FILE* file = nullptr;
    static bool opened = false;
    if (!opened) {
        opened = true;
        file = std::fopen(M1_LOG_PATH, "a");
        if (file) std::setvbuf(file, nullptr, _IOLBF, 0);
    }
    return file;
}

inline long threadId() {
#if defined(__linux__)
    return static_cast<long>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

inline double seconds() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#if defined(M1_DEBUG)

inline double sinceStart() {
    static const double start = seconds();
    return seconds() - start;
}

inline void log(const char* format, ...) {
    FILE* file = logFile();
    if (!file) return;
    std::fprintf(file, "[%ld %8.3f] ", threadId(), sinceStart());
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

#else

inline void log(const char*, ...) {}

#endif

inline long fileSize(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return -1;
    long size = -1;
    if (std::fseek(file, 0, SEEK_END) == 0) size = std::ftell(file);
    std::fclose(file);
    return size;
}

}  // namespace m1unit
