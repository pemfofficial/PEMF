// log.h - shared logging entry point.
#pragma once

// Defined in core.cpp. Writes a timestamped line to <game>\pemf.log
// and flushes immediately, so the log survives a hard crash.
void Log(const char* fmt, ...);
