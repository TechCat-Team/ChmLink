/* The application's logger. One per process; replaced under a lock that is
 * held while a message is delivered, so a callback never runs after
 * chmlink_set_logger replaced it (and must not call it itself). */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "log.h"

static SRWLOCK lock = SRWLOCK_INIT;
static chmlink_logger logger;
static void *logger_context;

void chmlink_set_logger(chmlink_logger callback, void *context)
{
    AcquireSRWLockExclusive(&lock);
    logger = callback; logger_context = context;
    ReleaseSRWLockExclusive(&lock);
}

static void deliver(chmlink_log_level level, const wchar_t *message)
{
    AcquireSRWLockShared(&lock);
    if (logger != NULL) logger(level, message, logger_context);
    ReleaseSRWLockShared(&lock);
}

static int formatted(wchar_t *out, size_t size, const wchar_t *format, va_list arguments)
{
    int length = _vsnwprintf_s(out, size, _TRUNCATE, format, arguments);
    return length < 0 ? (int)wcslen(out) : length;
}

void sdk_log(chmlink_log_level level, const wchar_t *format, ...)
{
    wchar_t message[512];
    va_list arguments;
    DWORD error = GetLastError();
    if (logger == NULL) return;   /* a racy read only skips formatting */
    va_start(arguments, format);
    formatted(message, 512, format, arguments);
    va_end(arguments);
    deliver(level, message);
    SetLastError(error);
}

void sdk_log_error(const wchar_t *format, ...)
{
    wchar_t message[768];
    va_list arguments;
    DWORD error = GetLastError(), length;
    int used;
    if (logger == NULL) return;
    va_start(arguments, format);
    used = formatted(message, 512, format, arguments);
    va_end(arguments);
    swprintf(message + used, 768 - used, L": ");
    used += 2;
    length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                            FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, error, 0, message + used, 768 - used - 24, NULL);
    while (length > 0 && (message[used + length - 1] == L' ' || message[used + length - 1] == L'.')) --length;
    used += (int)length;
    swprintf(message + used, 768 - used, L"%ls(error %lu)", length ? L" " : L"", error);
    deliver(CHMLINK_LOG_ERROR, message);
    SetLastError(error);
}
