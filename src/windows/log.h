/* Internal: messages to the application's logger (chmlink_set_logger). */
#ifndef CHMLINK_SDK_LOG_H
#define CHMLINK_SDK_LOG_H

#include <wchar.h>
#include "chmlink/api.h"

/* Both keep the calling thread's last error. */
void sdk_log(chmlink_log_level level, const wchar_t *format, ...);
/* An error message followed by the text and code of GetLastError(). */
void sdk_log_error(const wchar_t *format, ...);

#endif
