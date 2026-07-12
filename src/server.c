#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "mime.h"

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

#include <semaphore.h>

#define BUFFER_SIZE 8192
#define REQUEST_MAX 4096
#define RATE_TABLE_SIZE 256
#define RATE_WINDOW 60

static volatile int running = 1;
static sem_t thread_sem;
static int thread_limit_enabled = 0;

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
        if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
            uint32_t v4;
            memcpy(&v4, sin6->sin6_addr.s6_addr + 12, 4);
            return (uint32_t)ntohl(v4);
        }
        uint32_t parts[4];
        memcpy(parts, sin6->sin6_addr.s6_addr, 16);
        return parts[0] ^ parts[1] ^ parts[2] ^ parts[3];
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
    char ip_str[INET6_ADDRSTRLEN];
} client_job;

static const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Request Entity Too Large";
        case 416: return "Range Not Satisfiable";
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
        "Accept-Ranges: bytes\r\n"
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
                if (val == 0) { free(dst); return NULL; }
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

static void log_request(const char *ip, const char *method,
                        const char *path, int status, off_t size)
{
    fprintf(stderr, "[%s] %s %s -> %d (%lld bytes)\n",
            ip ? ip : "unknown", method ? method : "?", path ? path : "?",
            status, (long long)size);
}

static int is_path_safe(const char *path)
{
    if (strstr(path, "..")) return 0;
    if (strchr(path, '~')) return 0;
    if (path[0] != '/') return 0;
    return 1;
}

/* Parse Range header.  Returns 1 if valid, sets *start and *end.
   *end may be -1 meaning "to end of file".  When file_size is 0
   the caller just wants the raw values without validation. */
static int parse_range(const char *hdr, off_t file_size,
                       off_t *start, off_t *end)
{
    if (strncmp(hdr, "bytes=", 6) != 0) return 0;
    hdr += 6;

    char *dash = strchr(hdr, '-');
    if (!dash) return 0;

    if (dash == hdr) {
        long suffix = atol(hdr + 1);
        if (suffix <= 0) return 0;
        if (file_size == 0) { *start = *end = -1; return 1; }
        *start = (file_size > suffix) ? file_size - suffix : 0;
        *end = file_size - 1;
        return 1;
    }

    *start = atol(hdr);
    if (*start < 0 || (file_size > 0 && *start >= file_size)) return 0;

    if (*(dash + 1) == 0) {
        if (file_size > 0) *end = file_size - 1;
        else *end = -1;
    } else {
        *end = atol(dash + 1);
        if (*end < *start || (file_size > 0 && *end >= file_size)) return 0;
    }

    return 1;
}

static int serve_file(int fd, const char *path, const char *root_dir,
                      off_t *out_size, off_t range_start, off_t range_end)
{
    char full[8192];
    int n = snprintf(full, sizeof(full), "%s%s", root_dir, path);
    if (n < 0 || (size_t)n >= sizeof(full)) {
        send_error(fd, 500, "Path too long");
        return 500;
    }

    char *resolved = realpath(full, NULL);
    if (!resolved) {
        int code = (errno == EACCES) ? 403 : 404;
        send_error(fd, code, code == 403 ? "Access denied" : "File not found");
        return code;
    }
    if (strncmp(resolved, root_dir, strlen(root_dir)) != 0) {
        free(resolved);
        send_error(fd, 403, "Access denied");
        return 403;
    }
    size_t rlen = strlen(resolved);
    if (rlen >= sizeof(full)) {
        free(resolved);
        send_error(fd, 500, "Path too long");
        return 500;
    }
    memcpy(full, resolved, rlen + 1);
    free(resolved);

    struct stat st;
    if (stat(full, &st) < 0) {
        int code = (errno == EACCES) ? 403 : 404;
        send_error(fd, code, code == 403 ? "Access denied" : "File not found");
        return code;
    }

    if (S_ISDIR(st.st_mode)) {
        size_t len = strlen(full);
        if (full[len - 1] != '/') {
            char redirect[4096];
            snprintf(redirect, sizeof(redirect),
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                "<meta http-equiv=\"refresh\" content=\"0;url=%s/\">"
                "</head><body></body></html>", path);
            send_response(fd, 301, "text/html; charset=utf-8",
                          redirect, strlen(redirect));
            return 301;
        }

        char index_path[8192];
        int idx_n = snprintf(index_path, sizeof(index_path), "%sindex.html", full);
        if (idx_n < 0 || (size_t)idx_n >= sizeof(index_path)) {
            send_error(fd, 500, "Path too long");
            return 500;
        }

        if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) {
            memcpy(full, index_path, sizeof(full));
            char *res2 = realpath(full, NULL);
            if (!res2) {
                send_error(fd, 403, "Access denied");
                return 403;
            }
            if (strncmp(res2, root_dir, strlen(root_dir)) != 0) {
                free(res2);
                send_error(fd, 403, "Access denied");
                return 403;
            }
            size_t rl2 = strlen(res2);
            if (rl2 >= sizeof(full)) {
                free(res2);
                send_error(fd, 500, "Path too long");
                return 500;
            }
            memcpy(full, res2, rl2 + 1);
            free(res2);
        } else {
            send_error(fd, 403, "Directory listing not available");
            return 403;
        }
    }

    if (!S_ISREG(st.st_mode)) {
        send_error(fd, 403, "Not a regular file");
        return 403;
    }

    int file_fd = open(full, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 403, "Access denied");
        return 403;
    }

    off_t file_size = st.st_size;
    int is_range = (range_start >= 0);

    if (is_range && (range_start >= file_size || range_end >= file_size)) {
        close(file_fd);
        char body[256];
        int n = snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>416 Range Not Satisfiable</title></head><body>"
            "<h1>416 Range Not Satisfiable</h1>"
            "<p>Range 0-%lld</p></body></html>",
            (long long)(file_size - 1));

        char header[512];
        snprintf(header, sizeof(header),
            "HTTP/1.1 416 Range Not Satisfiable\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %d\r\n"
            "Content-Range: bytes */%lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n"
            "Server: OHTTPd/1.0\r\n"
            "\r\n",
            n, (long long)file_size);
        ssize_t r1 = write(fd, header, strlen(header));
        ssize_t r2 = write(fd, body, (size_t)n);
        (void)r1; (void)r2;
        return 416;
    }

    off_t send_start = is_range ? range_start : 0;
    off_t send_end   = is_range ? range_end : file_size - 1;
    off_t send_len   = send_end - send_start + 1;

    const char *ct = mime_type(full);

    if (lseek(file_fd, send_start, SEEK_SET) < 0) {
        close(file_fd);
        send_error(fd, 500, "Seek failed");
        return 500;
    }

    char header[1024];
    int hlen;
    if (is_range) {
        hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n"
            "Server: OHTTPd/1.0\r\n"
            "\r\n",
            ct, (long long)send_len,
            (long long)send_start, (long long)send_end,
            (long long)file_size);
    } else {
        hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n"
            "Server: OHTTPd/1.0\r\n"
            "\r\n",
            ct, (long long)file_size);
    }

    if (hlen < 0) { close(file_fd); send_error(fd, 500, "Header build failed"); return 500; }

    if (write(fd, header, (size_t)hlen) < 0) {
        close(file_fd);
        return -1;
    }

    off_t remaining = send_len;
    char buf[BUFFER_SIZE];
    while (remaining > 0) {
        size_t chunk = (remaining > (off_t)sizeof(buf)) ? sizeof(buf) : (size_t)remaining;
        ssize_t bytes_read = read(file_fd, buf, chunk);
        if (bytes_read <= 0) break;
        ssize_t written = 0;
        while (written < bytes_read) {
            ssize_t w = write(fd, buf + written, (size_t)(bytes_read - written));
            if (w < 0) { close(file_fd); return -1; }
            written += w;
        }
        remaining -= bytes_read;
    }

    close(file_fd);
    if (out_size) *out_size = send_len;
    return is_range ? 206 : 200;
}

static void handle_client(int fd, const char *root_dir, const char *ip_str)
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
        log_request(ip_str, NULL, NULL, 400, 0);
        close(fd);
        return;
    }

    char method[16], path[4096], version[16];
    int parsed = sscanf(buf, "%15s %4095s %15s", method, path, version);

    if (parsed < 2) {
        send_error(fd, 400, "Malformed request");
        log_request(ip_str, NULL, NULL, 400, 0);
        close(fd);
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_error(fd, 405, "Only GET and HEAD are supported");
        log_request(ip_str, method, path, 405, 0);
        close(fd);
        return;
    }

    if (parsed == 3) {
        if (strncmp(version, "HTTP/", 5) != 0) {
            send_error(fd, 400, "Invalid HTTP version");
            log_request(ip_str, method, path, 400, 0);
            close(fd);
            return;
        }
    }

    char *decoded = url_decode(path);
    if (!decoded) {
        send_error(fd, 400, "Invalid URL encoding");
        log_request(ip_str, method, path, 500, 0);
        close(fd);
        return;
    }

    if (!is_path_safe(decoded)) {
        send_error(fd, 403, "Path traversal detected");
        log_request(ip_str, method, decoded, 403, 0);
        free(decoded);
        close(fd);
        return;
    }

    off_t range_start = -1;
    off_t range_end = -1;
    const char *range_hdr = strstr(buf, "Range: ");
    if (range_hdr) {
        const char *eol = strchr(range_hdr, '\r');
        if (!eol) eol = strchr(range_hdr, '\n');
        if (eol) {
            char range_buf[256];
            size_t rlen = (size_t)(eol - range_hdr - 7);
            if (rlen < sizeof(range_buf)) {
                memcpy(range_buf, range_hdr + 7, rlen);
                range_buf[rlen] = 0;
                if (!parse_range(range_buf, 0, &range_start, &range_end))
                    range_start = -1;
            }
        }
    }

    off_t size = 0;
    int status = serve_file(fd, decoded, root_dir, &size, range_start, range_end);
    log_request(ip_str, method, decoded, status, size);
    free(decoded);
    close(fd);
}

static void *worker_thread(void *arg)
{
    client_job *job = (client_job *)arg;
    handle_client(job->fd, job->root_dir, job->ip_str);
    conn_dec();
    free(job);
    if (thread_limit_enabled)
        sem_post(&thread_sem);
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

    /* Resolve root directory to absolute path and check it exists */
    char *resolved_root = realpath(config->root_dir, NULL);
    if (!resolved_root) {
        fprintf(stderr, "error: root directory '%s' does not exist\n", config->root_dir);
        close(server_fd);
        return 1;
    }

    config->root_dir = resolved_root;

    if (config->max_threads > 0) {
        sem_init(&thread_sem, 0, (unsigned int)config->max_threads);
        thread_limit_enabled = 1;
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
            char drop_ip[INET6_ADDRSTRLEN] = "";
            if (client_addr.ss_family == AF_INET)
                inet_ntop(AF_INET, &((struct sockaddr_in *)&client_addr)->sin_addr, drop_ip, sizeof(drop_ip));
            else if (client_addr.ss_family == AF_INET6)
                inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&client_addr)->sin6_addr, drop_ip, sizeof(drop_ip));
            fprintf(stderr, "[%s] rate-limited\n", drop_ip);
            close(client_fd);
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (thread_limit_enabled)
            sem_wait(&thread_sem);

        conn_inc();

        client_job *job = malloc(sizeof(client_job));
        if (!job) {
            conn_dec();
            if (thread_limit_enabled)
                sem_post(&thread_sem);
            close(client_fd);
            continue;
        }

        job->fd = client_fd;
        job->root_dir = config->root_dir;
        job->ip_str[0] = 0;

        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&client_addr;
            inet_ntop(AF_INET, &sin->sin_addr, job->ip_str, sizeof(job->ip_str));
        } else if (client_addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&client_addr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, job->ip_str, sizeof(job->ip_str));
        }

        pthread_t thread;
        if (pthread_create(&thread, NULL, worker_thread, job) != 0) {
            perror("pthread_create");
            handle_client(client_fd, config->root_dir, job->ip_str);
            conn_dec();
            if (thread_limit_enabled)
                sem_post(&thread_sem);
            free(job);
        } else {
            pthread_detach(thread);
        }
    }

    printf("\nShutting down...\n");
    close(server_fd);
    return 0;
}
