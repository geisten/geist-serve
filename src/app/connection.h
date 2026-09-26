#pragma once
#include "core.h"
bool app_home(char out[static APP_PATH_CAP]);
bool app_key(const char *home, char token[static 65]);
bool app_connection_write(const char *home, unsigned port, const char *token);
bool app_connection_read(const char *home, unsigned *port, char token[static 65]);
void app_connection_remove(const char *home);
