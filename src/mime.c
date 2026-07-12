#include "mime.h"

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <strings.h>

const char *mime_type(const char *path)
{
    const char *p = strrchr(path, '.');
    if (!p) return "application/octet-stream";

    if (strcasecmp(p, ".html") == 0 || strcasecmp(p, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(p, ".css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(p, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(p, ".json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(p, ".png") == 0) return "image/png";
    if (strcasecmp(p, ".jpg") == 0 || strcasecmp(p, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(p, ".gif") == 0) return "image/gif";
    if (strcasecmp(p, ".svg") == 0 || strcasecmp(p, ".svgz") == 0) return "image/svg+xml";
    if (strcasecmp(p, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(p, ".webp") == 0) return "image/webp";
    if (strcasecmp(p, ".woff") == 0) return "font/woff";
    if (strcasecmp(p, ".woff2") == 0) return "font/woff2";
    if (strcasecmp(p, ".ttf") == 0) return "font/ttf";
    if (strcasecmp(p, ".otf") == 0) return "font/otf";
    if (strcasecmp(p, ".eot") == 0) return "application/vnd.ms-fontobject";
    if (strcasecmp(p, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(p, ".zip") == 0) return "application/zip";
    if (strcasecmp(p, ".gz") == 0) return "application/gzip";
    if (strcasecmp(p, ".tar") == 0) return "application/x-tar";
    if (strcasecmp(p, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(p, ".mp4") == 0) return "video/mp4";
    if (strcasecmp(p, ".webm") == 0) return "video/webm";
    if (strcasecmp(p, ".ogg") == 0) return "audio/ogg";
    if (strcasecmp(p, ".wav") == 0) return "audio/wav";
    if (strcasecmp(p, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(p, ".xml") == 0) return "application/xml; charset=utf-8";
    if (strcasecmp(p, ".yaml") == 0 || strcasecmp(p, ".yml") == 0) return "text/yaml; charset=utf-8";
    if (strcasecmp(p, ".md") == 0) return "text/markdown; charset=utf-8";

    return "application/octet-stream";
}
