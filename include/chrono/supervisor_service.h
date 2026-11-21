#ifndef CHRONO_SUPERVISOR_SERVICE_H
#define CHRONO_SUPERVISOR_SERVICE_H

#include <stddef.h>

/* Foreground service. Returns zero after a requested or signalled shutdown. */
int chrono_supervisor_service_run(const char *registry_directory,
                                  char *error, size_t error_size);

/* Send STATUS or SHUTDOWN to the current per-user service. */
int chrono_supervisor_service_request(const char *registry_directory,
                                     const char *request,
                                     char *response, size_t response_size,
                                     char *error, size_t error_size);

#endif
