# OHTTPd

A lightweight, single-binary HTTP/1.1 static file server written in C, licensed under the EUPL v1.2.

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
- Single ~400-line C99 codebase, no external dependencies beyond libc and pthreads

## Building

Requires a C99 compiler (GCC, Clang) and POSIX threads.

```sh
make          # builds the `ohttpd` binary
make clean    # removes object files and the binary
```

Override the compiler or flags:

```sh
make CC=clang CFLAGS="-Wall -Wextra -O2 -std=c99"
```

## Usage

```
Usage: ./ohttpd [options]

Options:
  -p, --port <port>      Port to listen on (default: 8080)
  -r, --root <dir>       Root directory for serving files (default: ./public)
  -b, --backlog <num>    Connection backlog (default: 16)
  -t, --threads <num>    Max worker threads (default: 4)
  -u, --user <name>      Drop privileges to this user after bind
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
```

Then open `http://localhost:8080/` in your browser.

## Project structure

```
OHTTPd/
├── Makefile        # Build system
├── src/
│   ├── main.c      # Entry point, CLI argument parsing
│   ├── server.h    # Public API header
│   ├── server.c    # Core server: sockets, HTTP, file serving, threading
│   ├── mime.h      # MIME type declaration
│   └── mime.c      # MIME type mapping table
├── public/
│   └── index.html  # Default landing page
├── LICENSE         # EUPL v1.2
└── README.md
```

## License

European Union Public Licence v. 1.2 — see [LICENSE](LICENSE).
