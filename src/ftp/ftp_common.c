/*
 * NC-Link core - shared FTP helpers: control connection line IO, reply
 * formatting, LIST formatting and parsing, timestamp conversion, path
 * normalisation.
 */
#include "ftp_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

/* --------------------------------------------------------------- reading -- */

void ncl_ftp_reader_init(ncl_ftp_reader *reader, ncl_socket *sock)
{
    if (reader == NULL) {
        return;
    }
    reader->sock = sock;
    reader->len = 0;
    reader->pos = 0;
}

ncl_err ncl_ftp_reader_line(ncl_ftp_reader *reader, ncl_strbuf *out,
                            unsigned timeout_ms)
{
    int64_t deadline;

    if (reader == NULL || out == NULL || reader->sock == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_strbuf_reset(out);
    deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    for (;;) {
        size_t i;
        unsigned remaining;
        int rc;

        for (i = reader->pos; i < reader->len; i++) {
            if (reader->buf[i] == '\n') {
                size_t end = i;
                size_t start = reader->pos;
                if (end > start && reader->buf[end - 1] == '\r') {
                    end--;
                }
                ncl_strbuf_append(out, reader->buf + start, end - start);
                reader->pos = i + 1;
                if (reader->pos >= reader->len) {
                    reader->pos = 0;
                    reader->len = 0;
                }
                return NCL_OK;
            }
        }
        /* No complete line buffered: move the tail down and read more. */
        if (reader->pos > 0) {
            size_t tail = reader->len - reader->pos;
            memmove(reader->buf, reader->buf + reader->pos, tail);
            reader->len = tail;
            reader->pos = 0;
        }
        if (reader->len >= sizeof(reader->buf)) {
            /* Overlong line: report what we have and resynchronise. */
            ncl_strbuf_append(out, reader->buf, reader->len);
            reader->len = 0;
            return NCL_OK;
        }
        {
            int64_t now = ncl_time_monotonic_millis();
            if (now >= deadline) {
                return NCL_ERR_TIMEOUT;
            }
            remaining = (unsigned)(deadline - now);
        }
        rc = ncl_socket_recv(reader->sock, reader->buf + reader->len,
                             sizeof(reader->buf) - reader->len, remaining);
        if (rc == NCL_SOCKET_TIMEOUT) {
            continue;
        }
        if (rc <= 0) {
            return NCL_ERR_IO;
        }
        reader->len += (size_t)rc;
    }
}

ncl_err ncl_ftp_reader_reply(ncl_ftp_reader *reader, int *code,
                             ncl_strbuf *text, unsigned timeout_ms)
{
    ncl_strbuf line;
    ncl_err rc;
    int expected = -1;

    if (reader == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_strbuf_init(&line);
    if (code != NULL) {
        *code = 0;
    }
    if (text != NULL) {
        ncl_strbuf_reset(text);
    }
    for (;;) {
        rc = ncl_ftp_reader_line(reader, &line, timeout_ms);
        if (rc != NCL_OK) {
            break;
        }
        {
            const char *s = ncl_strbuf_cstr(&line);
            int current = ncl_ftp_code_of(s);
            bool last;

            if (current < 0) {
                /* Not a reply line; skip it. */
                continue;
            }
            if (expected < 0) {
                expected = current;
                if (code != NULL) {
                    *code = current;
                }
            }
            last = s[3] == ' ' || s[3] == '\0';
            if (text != NULL) {
                const char *body = s[3] != '\0' ? s + 4 : "";
                if (ncl_strbuf_cstr(text)[0] != '\0') {
                    ncl_strbuf_putc(text, '\n');
                }
                ncl_strbuf_puts(text, body);
            }
            if (last) {
                break;
            }
        }
    }
    ncl_strbuf_free(&line);
    if (rc == NCL_OK && code != NULL && *code == 0) {
        rc = NCL_ERR_PARSE;
    }
    return rc;
}

int ncl_ftp_code_of(const char *line)
{
    if (line == NULL || line[0] < '0' || line[0] > '9' || line[1] < '0' ||
        line[1] > '9' || line[2] < '0' || line[2] > '9') {
        return -1;
    }
    return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

bool ncl_ftp_reply_is_completion(int code)
{
    return code >= 200 && code < 300;
}

bool ncl_ftp_reply_is_intermediate(int code)
{
    return code >= 300 && code < 400;
}

bool ncl_ftp_reply_is_preliminary(int code)
{
    return code >= 100 && code < 200;
}

/* --------------------------------------------------------------- writing -- */

ncl_err ncl_ftp_send_line(ncl_socket *sock, const char *line)
{
    ncl_strbuf sb;
    ncl_err rc;

    ncl_strbuf_init(&sb);
    ncl_strbuf_puts(&sb, line != NULL ? line : "");
    ncl_strbuf_puts(&sb, "\r\n");
    rc = ncl_socket_send(sock, ncl_strbuf_cstr(&sb), sb.len);
    ncl_strbuf_free(&sb);
    return rc;
}

ncl_err ncl_ftp_send_command(ncl_socket *sock, const char *fmt, ...)
{
    va_list ap;
    char *line = NULL;
    ncl_err rc;

    va_start(ap, fmt);
    rc = ncl_vasprintf(&line, fmt, ap);
    va_end(ap);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_ftp_send_line(sock, line);
    free(line);
    return rc;
}

ncl_err ncl_ftp_reply(ncl_socket *sock, int code, const char *text)
{
    return ncl_ftp_send_command(sock, "%d %s", code, text != NULL ? text : "");
}

ncl_err ncl_ftp_replyf(ncl_socket *sock, int code, const char *fmt, ...)
{
    va_list ap;
    char *body = NULL;
    ncl_err rc;

    va_start(ap, fmt);
    rc = ncl_vasprintf(&body, fmt, ap);
    va_end(ap);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_ftp_reply(sock, code, body);
    free(body);
    return rc;
}

ncl_err ncl_ftp_reply_multiline(ncl_socket *sock, int code, const char *text)
{
    const char *cursor = text != NULL ? text : "";
    const char *nl;
    ncl_err rc;

    for (;;) {
        nl = strchr(cursor, '\n');
        if (nl == NULL) {
            return ncl_ftp_reply(sock, code, cursor);
        }
        {
            char *piece = ncl_strndup(cursor, (size_t)(nl - cursor));
            if (piece == NULL) {
                return NCL_ERR_NOMEM;
            }
            rc = ncl_ftp_send_command(sock, "%d-%s", code, piece);
            free(piece);
        }
        if (rc != NCL_OK) {
            return rc;
        }
        cursor = nl + 1;
    }
}

/* ------------------------------------------------------------ port args -- */

ncl_err ncl_ftp_format_port_arg(char *out, size_t out_len, const char *host,
                                unsigned port)
{
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;
    int rc;

    if (out == NULL || out_len == 0 || host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d);
    if (rc != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        return NCL_ERR_INVALID_ARG;
    }
    snprintf(out, out_len, "%u,%u,%u,%u,%u,%u", a, b, c, d, port / 256,
             port % 256);
    return NCL_OK;
}

ncl_err ncl_ftp_parse_pasv_reply(const char *text, char *host, size_t host_len,
                                 unsigned *port)
{
    const char *open = NULL;
    const char *close;
    unsigned v[6];
    int rc;
    const char *cursor;
    size_t i;

    if (text == NULL || host == NULL || port == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /* The tuple sits in parentheses on the last line of the reply. */
    {
        const char *scan = text;
        while ((scan = strchr(scan, '(')) != NULL) {
            open = scan;
            scan++;
        }
    }
    if (open == NULL) {
        return NCL_ERR_PARSE;
    }
    close = strchr(open, ')');
    if (close == NULL) {
        return NCL_ERR_PARSE;
    }
    memset(v, 0, sizeof(v));
    cursor = open + 1;
    for (i = 0; i < 6; i++) {
        char *end = NULL;
        unsigned long value;
        while (*cursor == ' ') {
            cursor++;
        }
        value = strtoul(cursor, &end, 10);
        if (end == cursor || value > 255) {
            return NCL_ERR_PARSE;
        }
        v[i] = (unsigned)value;
        cursor = end;
        if (*cursor == ',') {
            cursor++;
        }
    }
    rc = snprintf(host, host_len, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    if (rc <= 0 || (size_t)rc >= host_len) {
        return NCL_ERR_RANGE;
    }
    *port = v[4] * 256 + v[5];
    return NCL_OK;
}

/* -------------------------------------------------------------- listings -- */

static const char *ncl_ftp_month_name(int month)
{
    static const char *names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (month < 0 || month > 11) {
        return "Jan";
    }
    return names[month];
}

ncl_err ncl_ftp_format_list_line(ncl_strbuf *out, const char *name, bool is_dir,
                                 long long size, int64_t mtime_ms)
{
    struct tm tm;
    char stamp[32];
    int64_t now_ms = ncl_time_millis();
    bool recent;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_ftp_localtime(mtime_ms, &tm);
    /* cmp: recent files show the time, older ones the year (ls convention). */
    recent = (now_ms - mtime_ms) < (int64_t)180 * 24 * 3600 * 1000 &&
             mtime_ms <= now_ms;
    if (recent) {
        snprintf(stamp, sizeof(stamp), "%s %2d %02d:%02d",
                 ncl_ftp_month_name(tm.tm_mon), tm.tm_mday, tm.tm_hour,
                 tm.tm_min);
    } else {
        snprintf(stamp, sizeof(stamp), "%s %2d  %4d",
                 ncl_ftp_month_name(tm.tm_mon), tm.tm_mday, tm.tm_year + 1900);
    }
    return ncl_strbuf_printf(out, "%s 1 ftp ftp %12lld %s %s",
                             is_dir ? "drwxr-xr-x" : "-rw-r--r--", size, stamp,
                             name != NULL ? name : "");
}

ncl_err ncl_ftp_parse_list_line(const char *line, ncl_ftp_entry *out)
{
    const char *cursor;
    long long size = 0;
    int64_t when = 0;
    int month = -1;
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    bool have_time = false;
    char name[NCL_FTP_PATH_MAX];

    if (line == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (line[0] == '\0' || strncmp(line, "total ", 6) == 0) {
        return NCL_ERR_PARSE;
    }
    out->type = (line[0] == 'd') ? 1 : 0;
    /* Unix format: mode links owner group size month day time/year name */
    cursor = line;
    {
        const char *p = cursor;
        int field = 0;
        /* Skip the permission block plus the four numeric/name columns by
         * walking whitespace separated tokens. */
        while (*p != '\0' && field < 4) {
            while (*p != '\0' && *p != ' ') {
                p++;
            }
            while (*p == ' ') {
                p++;
            }
            field++;
        }
        cursor = p;
    }
    {
        char *end = NULL;
        size = strtoll(cursor, &end, 10);
        if (end == cursor) {
            return NCL_ERR_PARSE;
        }
        cursor = end;
        while (*cursor == ' ') {
            cursor++;
        }
    }
    {
        /* month day (hh:mm | yyyy) */
        int i;
        for (i = 0; i < 12; i++) {
            if (ncl_strncasecmp(cursor, ncl_ftp_month_name(i), 3) == 0) {
                month = i;
                break;
            }
        }
        if (month < 0) {
            return NCL_ERR_PARSE;
        }
        cursor += 3;
        while (*cursor == ' ') {
            cursor++;
        }
        day = (int)strtol(cursor, (char **)&cursor, 10);
        while (*cursor == ' ') {
            cursor++;
        }
        if (strchr(cursor, ':') != NULL) {
            char *end = NULL;
            hour = (int)strtol(cursor, &end, 10);
            cursor = end;
            if (*cursor == ':') {
                cursor++;
            }
            minute = (int)strtol(cursor, &end, 10);
            cursor = end;
            have_time = true;
        } else {
            char *end = NULL;
            year = (int)strtol(cursor, &end, 10);
            cursor = end;
        }
    }
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '\0') {
        return NCL_ERR_PARSE;
    }
    snprintf(name, sizeof(name), "%s", cursor);

    if (have_time) {
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        ncl_ftp_localtime(ncl_time_millis(), &tm);
        tm.tm_mon = month;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = 0;
        /* The year is implicit for recent entries; assume the current one. */
        when = ncl_ftp_mktime_local(&tm);
    } else {
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = year - 1900;
        tm.tm_mon = month;
        tm.tm_mday = day;
        when = ncl_ftp_mktime_local(&tm);
    }
    out->name = ncl_strdup(name);
    if (out->name == NULL) {
        return NCL_ERR_NOMEM;
    }
    out->size = size;
    out->modify_time = when;
    return NCL_OK;
}

/* --------------------------------------------------------------- time ----- */

void ncl_ftp_localtime(int64_t ms, struct tm *out)
{
    time_t seconds = (time_t)(ms / 1000);
    memset(out, 0, sizeof(*out));
#if defined(NCL_OS_WINDOWS)
    localtime_s(out, &seconds);
#else
    localtime_r(&seconds, out);
#endif
}

int64_t ncl_ftp_mktime_local(const struct tm *tm)
{
    struct tm copy = *tm;
    time_t seconds = mktime(&copy);
    if (seconds == (time_t)-1) {
        return 0;
    }
    return (int64_t)seconds * 1000;
}

/**
 * Civil date to Unix time (UTC), independent of timegm()/mktime() and of the
 * machine time zone. Uses Howard Hinnant's days-from-civil algorithm, so it
 * behaves the same on Windows and on POSIX without special feature macros.
 */
int64_t ncl_ftp_utc_to_epoch(int year, int month, int day, int hour, int minute,
                             int second)
{
    int64_t era;
    int64_t year_of_era;
    int64_t day_of_year;
    int64_t day_of_era;
    int64_t days;
    int y = year;

    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return -1;
    }
    y -= month <= 2 ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    year_of_era = (int64_t)y - era * 400;
    day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    days = era * 146097 + day_of_era - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

int64_t ncl_ftp_parse_mdtm(const char *text)
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int64_t seconds;

    if (text == NULL || strlen(text) < 14) {
        return 0;
    }
    while (*text == ' ') {
        text++;
    }
    if (sscanf(text, "%4d%2d%2d%2d%2d%2d", &year, &month, &day, &hour, &minute,
               &second) != 6) {
        return 0;
    }
    seconds = ncl_ftp_utc_to_epoch(year, month, day, hour, minute, second);
    if (seconds == (int64_t)-1) {
        return 0;
    }
    return seconds * 1000;
}

void ncl_ftp_format_mdtm(int64_t ms, char *out, size_t out_len)
{
    time_t seconds = (time_t)(ms / 1000);
    struct tm tm;

    memset(&tm, 0, sizeof(tm));
#if defined(NCL_OS_WINDOWS)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    snprintf(out, out_len, "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ---------------------------------------------------------------- paths --- */

void ncl_ftp_entry_free(void *entry)
{
    ncl_ftp_entry *e = (ncl_ftp_entry *)entry;
    if (e == NULL) {
        return;
    }
    free(e->name);
    free(e);
}

bool ncl_ftp_entry_is_dir(const ncl_ftp_entry *entry)
{
    return entry != NULL && entry->type == 1;
}

ncl_err ncl_ftp_path_join(char *out, size_t out_len, const char *dir,
                          const char *name)
{
    int rc;
    size_t dir_len;

    if (out == NULL || out_len == 0 || name == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (dir == NULL || dir[0] == '\0' || strcmp(dir, "/") == 0) {
        rc = snprintf(out, out_len, "/%s", name);
    } else {
        dir_len = strlen(dir);
        if (dir[dir_len - 1] == '/') {
            dir_len--;
        }
        rc = snprintf(out, out_len, "%.*s/%s", (int)dir_len, dir, name);
    }
    if (rc <= 0 || (size_t)rc >= out_len) {
        return NCL_ERR_RANGE;
    }
    return NCL_OK;
}

ncl_err ncl_ftp_path_normalise(const char *cwd, const char *path, char *out,
                               size_t out_len)
{
    char work[NCL_FTP_PATH_MAX];
    char *segments[128];
    size_t depth = 0;
    const char *base;
    const char *p;
    char *cursor;
    size_t i;
    size_t used = 0;
    int rc;

    if (out == NULL || out_len == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    if (path == NULL || path[0] == '\0') {
        path = "/";
    }
    base = (path[0] == '/')
               ? ""
               : ((cwd != NULL && cwd[0] != '\0') ? cwd : "/");
    /* Backslashes are tolerated so that a client sending Windows-ish paths on
     * this protocol still lands inside the root. */
    {
        size_t written = 0;
        for (p = path; *p != '\0' && written + 1 < sizeof(work); p++) {
            work[written++] = (*p == '\\') ? '/' : *p;
        }
        work[written] = '\0';
    }

    cursor = work;
    while (*cursor != '\0' && depth < sizeof(segments) / sizeof(segments[0])) {
        char *slash;
        while (*cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        slash = strchr(cursor, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
        if (strcmp(cursor, ".") == 0) {
            /* nothing */
        } else if (strcmp(cursor, "..") == 0) {
            if (depth > 0) {
                depth--;
            }
        } else {
            segments[depth++] = cursor;
        }
        if (slash == NULL) {
            break;
        }
        cursor = slash + 1;
    }

    /* Rebuild: the base (the current directory for relative paths) comes
     * first, then the normalised segments. */
    out[0] = '\0';
    if (base[0] != '\0' && strcmp(base, "/") != 0) {
        size_t base_len = strlen(base);
        while (base_len > 1 && base[base_len - 1] == '/') {
            base_len--;
        }
        if (base_len >= out_len) {
            return NCL_ERR_RANGE;
        }
        memcpy(out, base, base_len);
        out[base_len] = '\0';
        used = base_len;
    }
    for (i = 0; i < depth; i++) {
        rc = snprintf(out + used, out_len - used, "/%s", segments[i]);
        if (rc <= 0 || (size_t)rc >= out_len - used) {
            return NCL_ERR_RANGE;
        }
        used += (size_t)rc;
    }
    if (used == 0) {
        if (out_len < 2) {
            return NCL_ERR_RANGE;
        }
        out[0] = '/';
        out[1] = '\0';
    }
    return NCL_OK;
}
