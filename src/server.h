#ifndef SERVER_H
#define SERVER_H

typedef struct {
    char *root_dir;
    int port;
    int backlog;
    int max_threads;
    const char *run_user;
    int max_connections;
    int rate_limit;
} server_config;

int server_start(server_config *config);

#endif
