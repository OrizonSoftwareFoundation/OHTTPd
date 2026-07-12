#include "server.h"
#include "mime.h"

#define _POSIX_C_SOURCE 200809L

#include <stdatomic.h>
#include <stdint.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define BUFFER_SIZE 8192
#define REQUEST_MAX 4096
#define RATE_TABLE_SIZE 256
#define RATE_WINDOW 60

static volatile int running = 1;

/* --- rate limiter --- */

typedef struct {
    uint32_t ip;
    time_t window_start;
    unsigned int count;
} rate_entry;

static rate_entry rate_table[RATE_TABLE_SIZE];
static pthread_mutex_t rate_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t ip4_from_addr(const struct sockaddr_storage *addr)
{
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        return (uint32_t)ntohl(sin->sin_addr.s_addr);
    }
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
            return (uint32_t)(ntohl(sin6->sin6_addr.__in6_u.__u6_addr32[3]));
    }
    return 0;
}

static int rate_check(uint32_t ip, int limit)
{
    if (limit <= 0) return 1;

    time_t now = time(NULL);
    unsigned int slot = (ip ^ (ip >> 8)) % RATE_TABLE_SIZE;

    pthread_mutex_lock(&rate_lock);

    rate_entry *e = &rate_table[slot];
    if (e->ip != ip || (now - e->window_start) > RATE_WINDOW) {
        e->ip = ip;
        e->window_start = now;
        e->count = 1;
        pthread_mutex_unlock(&rate_lock);
        return 1;
    }

    e->count++;
    int ok = e->count <= (unsigned int)limit;
    pthread_mutex_unlock(&rate_lock);
    return ok;
}

/* --- connection limit --- */

static atomic_int active_connections = 0;

static void conn_inc(void)
{
    atomic_fetch_add(&active_connections, 1);
}

static void conn_dec(void)
{
    atomic_fetch_sub(&active_connections, 1);
}

static int conn_try_accept(int max)
{
    if (max <= 0) return 1;
    int cur = atomic_load(&active_connections);
    return cur < max;
}

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

typedef struct {
    int fd;
    char *root_dir;
} client_job;

static const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Request Entity Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default:  return "Unknown";
    }
}

static int send_response(int fd, int code, const char *content_type,
                         const char *body, size_t body_len)
{
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Server: OHTTPd/1.0\r\n"
        "\r\n",
        code, status_text(code),
        content_type ? content_type : "text/plain; charset=utf-8",
        body_len);

    if (n < 0) return -1;

    if (write(fd, header, (size_t)n) < 0) return -1;

    if (body && body_len > 0) {
        if (write(fd, body, body_len) < 0) return -1;
    }

    return 0;
}

static int send_error(int fd, int code, const char *msg)
{
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>%d %s</title></head><body>"
        "<h1>%d %s</h1><p>%s</p>"
        "<hr><p><small>OHTTPd/1.0</small></p>"
        "</body></html>",
        code, status_text(code), code, status_text(code), msg);

    return send_response(fd, code, "text/html; charset=utf-8", body, (size_t)n);
}

static char *url_decode(const char *src)
{
    size_t len = strlen(src);
    char *dst = malloc(len + 1);
    if (!dst) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len) {
            char hex[3] = {src[i+1], src[i+2], 0};
            char *end;
            long val = strtol(hex, &end, 16);
            if (*end == 0) {
                dst[j++] = (char)val;
                i += 2;
            } else {
                dst[j++] = src[i];
            }
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = 0;
    return dst;
}

static int drop_privileges(const char *user)
{
    if (!user) return 0;

    struct passwd pw;
    struct passwd *result;
    char pwbuf[4096];

    int r = getpwnam_r(user, &pw, pwbuf, sizeof(pwbuf), &result);
    if (r != 0 || !result) {
        fprintf(stderr, "error: cannot find user '%s'\n", user);
        return -1;
    }

    if (initgroups(user, pw.pw_gid) < 0) {
        perror("initgroups");
        return -1;
    }

    if (setgid(pw.pw_gid) < 0) {
        perror("setgid");
        return -1;
    }

    if (setuid(pw.pw_uid) < 0) {
        perror("setuid");
        return -1;
    }

    printf("Privileges dropped to user '%s' (uid=%d gid=%d)\n", user,
           pw.pw_uid, pw.pw_gid);
    return 0;
}

static int is_path_safe(const char *path)
{
    if (strstr(path, "..")) return 0;
    if (strchr(path, '~')) return 0;
    if (path[0] != '/') return 0;
    return 1;
}

static int serve_file(int fd, const char *path, const char *root_dir)
{
    char full[8192];
    int n = snprintf(full, sizeof(full), "%s%s", root_dir, path);
    if (n < 0 || (size_t)n >= sizeof(full)) return send_error(fd, 500, "Path too long");

    struct stat st;
    if (stat(full, &st) < 0) {
        if (errno == EACCES) return send_error(fd, 403, "Access denied");
        return send_error(fd, 404, "File not found");
    }

    if (S_ISDIR(st.st_mode)) {
        size_t len = strlen(full);
        if (full[len - 1] != '/') {
            char redirect[4096];
            snprintf(redirect, sizeof(redirect),
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                "<meta http-equiv=\"refresh\" content=\"0;url=%s/\">"
                "</head><body></body></html>", path);
            return send_response(fd, 301, "text/html; charset=utf-8",
                                 redirect, strlen(redirect));
        }

        char index_path[8192];
        int idx_n = snprintf(index_path, sizeof(index_path), "%sindex.html", full);
        if (idx_n < 0 || (size_t)idx_n >= sizeof(index_path))
            return send_error(fd, 500, "Path too long");

        if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) {
            memcpy(full, index_path, sizeof(full));
        } else {
            return send_error(fd, 403, "Directory listing not available");
        }
    }

    if (!S_ISREG(st.st_mode)) {
        return send_error(fd, 403, "Not a regular file");
    }

    int file_fd = open(full, O_RDONLY);
    if (file_fd < 0) {
        return send_error(fd, 403, "Access denied");
    }

    off_t file_size = st.st_size;
    const char *ct = mime_type(full);

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n"
        "Server: OHTTPd/1.0\r\n"
        "\r\n",
        ct, (long long)file_size);

    if (hlen < 0) { close(file_fd); return send_error(fd, 500, "Header build failed"); }

    if (write(fd, header, (size_t)hlen) < 0) {
        close(file_fd);
        return -1;
    }

    char buf[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(file_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < bytes_read) {
            ssize_t w = write(fd, buf + written, (size_t)(bytes_read - written));
            if (w < 0) { close(file_fd); return -1; }
            written += w;
        }
    }

    close(file_fd);
    return 0;
}

static void handle_client(int fd, const char *root_dir)
{
    char buf[REQUEST_MAX];
    ssize_t received = read(fd, buf, sizeof(buf) - 1);

    if (received < 0) {
        close(fd);
        return;
    }

    buf[received] = 0;

    if (memchr(buf, 0, (size_t)received) != NULL) {
        send_error(fd, 400, "Null byte in request");
        close(fd);
        return;
    }

    char method[16], path[4096], version[16];
    int parsed = sscanf(buf, "%15s %4095s %15s", method, path, version);

    if (parsed < 2) {
        send_error(fd, 400, "Malformed request");
        close(fd);
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_error(fd, 405, "Only GET and HEAD are supported");
        close(fd);
        return;
    }

    if (parsed == 3) {
        if (strncmp(version, "HTTP/", 5) != 0) {
            send_error(fd, 400, "Invalid HTTP version");
            close(fd);
            return;
        }
    }

    char *decoded = url_decode(path);
    if (!decoded) {
        send_error(fd, 500, "URL decode failed");
        close(fd);
        return;
    }

    if (!is_path_safe(decoded)) {
        send_error(fd, 403, "Path traversal detected");
        free(decoded);
        close(fd);
        return;
    }

    serve_file(fd, decoded, root_dir);
    free(decoded);
    close(fd);
}

static void *worker_thread(void *arg)
{
    client_job *job = (client_job *)arg;
    handle_client(job->fd, job->root_dir);
    conn_dec();
    free(job);
    return NULL;
}

static int create_server_socket(int port, int backlog)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            return -1;
        }
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(fd);
        return -1;
    }

#if defined(IPV6_V6ONLY)
    opt = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons((uint16_t)port);
    addr6.sin6_addr = in6addr_any;

    if (bind(fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons((uint16_t)port);
        addr4.sin_addr.s_addr = INADDR_ANY;

        if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
            perror("bind");
            close(fd);
            return -1;
        }
    }

    if (listen(fd, backlog) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int server_start(server_config *config)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);

    int server_fd = create_server_socket(config->port, config->backlog);
    if (server_fd < 0) return 1;

    if (drop_privileges(config->run_user) < 0) {
        close(server_fd);
        return 1;
    }

    printf("OHTTPd/1.0 — listening on http://0.0.0.0:%d/\n", config->port);
    printf("Root directory: %s\n", config->root_dir);
    printf("Max threads: %d\n", config->max_threads);
    if (config->max_connections > 0)
        printf("Max connections: %d\n", config->max_connections);
    if (config->rate_limit > 0)
        printf("Rate limit: %d/min per IP\n", config->rate_limit);
    printf("Press Ctrl+C to stop.\n");

    while (running) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        if (!conn_try_accept(config->max_connections)) {
            close(client_fd);
            continue;
        }

        uint32_t ip = ip4_from_addr(&client_addr);
        if (!rate_check(ip, config->rate_limit)) {
            close(client_fd);
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        conn_inc();

        client_job *job = malloc(sizeof(client_job));
        if (!job) {
            conn_dec();
            close(client_fd);
            continue;
        }

        job->fd = client_fd;
        job->root_dir = config->root_dir;

        pthread_t thread;
        if (pthread_create(&thread, NULL, worker_thread, job) != 0) {
            perror("pthread_create");
            handle_client(client_fd, config->root_dir);
            conn_dec();
            free(job);
        } else {
            pthread_detach(thread);
        }
    }

    printf("\nShutting down...\n");
    close(server_fd);
    return 0;
}
