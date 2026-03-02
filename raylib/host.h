#pragma once
#include <stdbool.h>

// Start embedded server on a background thread. Returns 0 on success.
int host_start(int port);

// Stop the server thread and close all sockets.
void host_stop(void);

// Returns true if the server is currently running.
bool host_is_running(void);
