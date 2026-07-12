#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -p, --port <port>      Port to listen on (default: 8080)\n"
        "  -r, --root <dir>       Root directory for serving files\n"
        "                         (default: ./public)\n"
        "  -b, --backlog <num>    Connection backlog (default: 16)\n"
        "  -t, --threads <num>    Max worker threads (default: 4)\n"
        "  -u, --user <name>      Drop privileges to this user after bind\n"
        "  -c, --connlimit <num>  Max concurrent connections (default: 256, 0=unlim)\n"
        "  -l, --ratelimit <num>  Max connections/min per IP (default: 100, 0=unlim)\n"
        "  -h, --help             Show this help and exit\n",
        prog);
}

int main(int argc, char *argv[])
{
    server_config config;
    config.port = 8080;
    config.root_dir = "./public";
    config.backlog = 16;
    config.max_threads = 4;
    config.run_user = NULL;
    config.max_connections = 256;
    config.rate_limit = 600;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --port requires an argument\n");
                return 1;
            }
            config.port = atoi(argv[++i]);
            if (config.port <= 0 || config.port > 65535) {
                fprintf(stderr, "error: invalid port %d\n", config.port);
                return 1;
            }
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--root") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --root requires an argument\n");
                return 1;
            }
            config.root_dir = argv[++i];
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--backlog") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --backlog requires an argument\n");
                return 1;
            }
            config.backlog = atoi(argv[++i]);
            if (config.backlog <= 0) {
                fprintf(stderr, "error: backlog must be positive\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --threads requires an argument\n");
                return 1;
            }
            config.max_threads = atoi(argv[++i]);
            if (config.max_threads <= 0) {
                fprintf(stderr, "error: threads must be positive\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--user") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --user requires an argument\n");
                return 1;
            }
            config.run_user = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--connlimit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --connlimit requires an argument\n");
                return 1;
            }
            config.max_connections = atoi(argv[++i]);
            if (config.max_connections < 0) {
                fprintf(stderr, "error: connlimit must be >= 0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--ratelimit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --ratelimit requires an argument\n");
                return 1;
            }
            config.rate_limit = atoi(argv[++i]);
            if (config.rate_limit < 0) {
                fprintf(stderr, "error: ratelimit must be >= 0\n");
                return 1;
            }
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    return server_start(&config);
}
