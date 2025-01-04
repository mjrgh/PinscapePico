// Pinscape Pico - PWM Worker - logger
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdio.h>
#include <stdarg.h>
#include <pico/stdlib.h>
#include "Logger.h"


void LogV(LogLevel level, const char *f, va_list va)
{
    // there's nothing to do if the string is null or empty
    if (f == nullptr || f[0] == 0)
        return;

    // show the level
    switch (level)
    {
    case LOG_INFO:
        printf("[Info]    ");
        break;

    case LOG_WARNING:
        printf("[Warning] ");
        break;

    case LOG_ERROR:
        printf("[Error]   ");
        break;
    }

    // show the message
    vprintf(f, va);
}

void Log(LogLevel level, const char *msg, ...)
{
    // pass the varargs through to the va_list function
    va_list va;
    va_start(va, msg);
    LogV(level, msg, va);
    va_end(va);
}



