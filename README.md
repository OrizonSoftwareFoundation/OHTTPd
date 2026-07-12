# OHTTPd

A lightweight, single-binary HTTP static file server written in C, licensed under the EUPL v1.2.

## Features

- HTTP/1.1 GET and HEAD requests
- Static file serving with automatic `index.html` resolution
- IPv4/IPv6 dual-stack
- Concurrent connections via pthreads
- Path traversal protection
- URL percent-decoding
- MIME type detection for 30+ file extensions
- Clean HTML error pages (400, 403, 404, 405, 413, 500, 501)
- Graceful shutdown on SIGINT/SIGTERM
- PIE + stack-protector + FORTIFY compile-time hardening
- Privilege drop after bind (`-u nobody`)
- 5-second socket receive timeout (slow-loris mitigation)
- Null byte and path traversal rejection
- Per-IP rate limiting (default 10 req/s, configurable)
- Global connection limit (default 256 concurrent)
- Per-request logging to stderr (IP, method, path, status, size)
- Single ~500-line C99 codebase, no external dependencies beyond libc and pthreads

## Building

Requires a GCC or Clang 
Make/Make clean is implemented

## Usage

```
Usage: ./ohttpd [options]

Options:
  -p, --port <port>      Port to listen on (default: 8080)
  -r, --root <dir>       Root directory for serving files (default: ./public)
  -b, --backlog <num>    Connection backlog (default: 16)
  -t, --threads <num>    Max worker threads (default: 4)
  -u, --user <name>      Drop privileges to this user after bind
  -c, --connlimit <num>  Max concurrent connections (default: 256, 0=unlim)
  -l, --ratelimit <num>  Max connections/min per IP (default: 600, 0=unlim)
  -h, --help             Show this help and exit
```

### Quick start

```sh
# Build
make

# Run with the default ./public directory
./ohttpd

# Run on a custom port and directory
./ohttpd -p 3000 -r /var/www/html

# Run with more worker threads
./ohttpd -p 80 -t 8

# Run on port 80 and drop privileges to nobody
sudo ./ohttpd -p 80 -u nobody -r /var/www/html


European Union Public Licence v. 1.2 - see [LICENSE](LICENSE).
