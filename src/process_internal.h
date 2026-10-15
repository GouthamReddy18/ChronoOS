#ifndef CHRONO_PROCESS_INTERNAL_H
#define CHRONO_PROCESS_INTERNAL_H

#include <sys/types.h>

int chrono_parse_uid_line(const char *text, uid_t *owner);

#endif
