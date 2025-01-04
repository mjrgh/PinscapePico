// Pinscape Pico - PWM Worker - logger
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdio.h>
#include <pico/stdlib.h>

// log level
enum LogLevel { LOG_INFO, LOG_WARNING, LOG_ERROR };

// log a message
void LogV(LogLevel level, const char *f, va_list va);
void Log(LogLevel level, const char *msg, ...);
