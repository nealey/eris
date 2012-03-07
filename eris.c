/*
 * simple httpd to be started from tcpserver 
 */
#define _FILE_OFFSET_BITS 64
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <grp.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <dirent.h>
#include <limits.h>

#ifdef __linux__
#   include <sys/sendfile.h>
#else
#   define sendfile(a, b, c, d) -1
#endif

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

/*
 * Some things I use for debugging 
 */
#define DUMPf(fmt, args...) fprintf(stderr, "%s:%s:%d " fmt "\n", __FILE__, __FUNCTION__, __LINE__, ##args)
#define DUMP() DUMPf("")
#define DUMP_d(v) DUMPf("%s = %d", #v, v)
#define DUMP_u(v) DUMPf("%s = %u", #v, v)
#define DUMP_x(v) DUMPf("%s = 0x%x", #v, v)
#define DUMP_s(v) DUMPf("%s = %s", #v, v)
#define DUMP_c(v) DUMPf("%s = %c", #v, v)
#define DUMP_p(v) DUMPf("%s = %p", #v, v)
#define DUMP_buf(v, l) DUMPf("%s = %.*s", #v, (int)(l), v)

/* Wait this long (seconds) for a valid HTTP request */
#define READTIMEOUT 2

/* Wait this long trying to write out the response */
#define WRITETIMEOUT 20

/* Wait this long for CGI to complete */
#define CGI_TIMEOUT	(5*60)

/* Maximum size of a request header (the whole block) */
#define MAXHEADERLEN 8192

/* Maximum size of a request line */
#define MAXREQUESTLEN 2048

/* Maximum number of header fields */
#define MAXHEADERFIELDS 60

/*
 * Options
 */
int             doauth = 0;
int             docgi = 0;
int             dirlist = 0;
int             redirect = 0;
int             portappend = 0;

/* Variables that persist between requests */
int             keepalive = 0;
char           *remote_ip = NULL;
char           *remote_port = NULL;
char           *remote_ident = NULL;

/* Things that are really super convenient to have globally */
char *host;
char *user_agent;
char *refer;
char *path;
int   http_version;

static const char days[] = "SunMonTueWedThuFriSat";
static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

#define BUFFER_SIZE 8192
char            stdout_buf[BUFFER_SIZE];

/*
 * TCP_CORK is a Linux extension to work around a TCP problem.
 * http://www.baus.net/on-tcp_cork has a good description.
 * XXX:  Since we do our own buffering, TCP_CORK may not be helping
 * with anything.  This needs testing.
 */
void
cork(int enable)
{
#ifdef TCP_CORK
    static int corked = 0;

    if (enable != corked) {
        setsockopt(1, IPPROTO_TCP, TCP_CORK, &enable, sizeof(enable));
        corked = enable;
    }
#endif
}


/** Replace whitespace with underscores for logging */
static void
sanitize(char *s)
{
    if (!s) {
        return;
    }
    for (; *s; s += 1) {
        if (isspace(*s)) {
            *s = '_';
        }
    }
}

/** Log a request */
static void
dolog(int code, off_t len)
{                               /* write a log line to stderr */
    sanitize(host);
    sanitize(user_agent);
    sanitize(refer);

    fprintf(stderr, "%s %d %lu %s %s %s %s\n",
            remote_ip, code, (unsigned long) len, host, user_agent, refer, path);
}

/*
 * output an error message and exit 
 */
static void
badrequest(long code, const char *httpcomment, const char *message)
{
    size_t msglen = 0;

    printf("HTTP/1.0 %ld %s\r\nConnection: close\r\n", code, httpcomment);
    if (message) {
        msglen = (strlen(message) * 2) + 15;

        printf("Content-Length: %lu\r\nContent-Type: text/html\r\n\r\n",
               (unsigned long) msglen);
        printf("<title>%s</title>%s", message, message);
    } else {
        fputs("\r\n", stdout);
    }
    fflush(stdout);
    dolog(code, msglen);

    exit(0);
}


/** Parse a header out of a line.
 *
 * This capitalizes the header name, and strips trailing [\r\n]
 * Returns the length of the line (after stripping)
 */
size_t
extract_header_field(char *buf, char **val, int cgi)
{
    size_t len;

    *val = NULL;

    for (len = 0; buf[len]; len += 1) {
        if (! *val) {
            if (buf[len] == ':') {
                buf[len] = 0;
                for (*val = &(buf[len+1]); **val == ' '; *val += 1);
            } else if (cgi) {
                switch (buf[len]) {
                    case 'a'...'z':
                        buf[len] ^= ' ';
                        break;
                    case 'A'...'Z':
                    case '0'...'9':
                    case '\r':
                    case '\n':
                        break;
                    default:
                        buf[len] = '_';
                        break;
                }
            }
        }
    }

    for (; (buf[len-1] == '\n') || (buf[len-1] == '\r'); len -= 1);
    buf[len] = 0;

    return len;
}


int
fromhex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else {
        c |= ' ';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
    }
    return -1;
}

/*
 * header(buf,buflen,"User-Agent")="Mozilla" 
 */
static char    *
header(char *buf, int buflen, const char *hname)
{
    int             slen = strlen(hname);
    int             i;
    char           *c;

    for (i = 0; i < buflen - slen - 2; ++i) {
        if (!strncasecmp(buf + i, hname, slen)) {
            if (i && (buf[i - 1] && buf[i - 1] != '\n'))
                continue;
            if (buf[i + slen] != ':' || buf[i + slen + 1] != ' ')
                continue;
            c = buf + i + slen + 2;
            i += slen + 2;
            for (; i < buflen; ++i) {
                if (buf[i] == 0 || buf[i] == '\n' || buf[i] == '\r') {
                    buf[i] = 0;
                    break;
                }
            }
            while (*c == ' ' || *c == '\t')
                ++c;
            return c;
        }
    }
    return 0;
}

static struct mimeentry {
    const char     *name,
                   *type;
} mimetab[] = {
    {
    "html", "text/html; charset=UTF-8"}, {
    "htm", "text/html; charset=UTF-8"}, {
    "txt", "text/plain; charset=UTF-8"}, {
    "css", "text/css"}, {
    "ps", "application/postscript"}, {
    "pdf", "application/pdf"}, {
    "js", "application/javascript"}, {
    "gif", "image/gif"}, {
    "png", "image/png"}, {
    "jpeg", "image/jpeg"}, {
    "jpg", "image/jpeg"}, {
    "svg", "image/svg+xml"}, {
    "mpeg", "video/mpeg"}, {
    "mpg", "video/mpeg"}, {
    "avi", "video/x-msvideo"}, {
    "mov", "video/quicktime"}, {
    "qt", "video/quicktime"}, {
    "mp3", "audio/mpeg"}, {
    "ogg", "audio/ogg"}, {
    "wav", "audio/x-wav"}, {
    "epub", "application/epub+zip"}, {
    "dvi", "application/x-dvi"}, {
    "pac", "application/x-ns-proxy-autoconfig"}, {
    "sig", "application/pgp-signature"}, {
    "swf", "application/x-shockwave-flash"}, {
    "torrent", "application/x-bittorrent"}, {
    "tar", "application/x-tar"}, {
    "zip", "application/zip"}, {
    "dtd", "text/xml"}, {
    "xml", "text/xml"}, {
    "xbm", "image/x-xbitmap"}, {
    "xpm", "image/x-xpixmap"}, {
    "xwd", "image/x-xwindowdump"}, {
    "ico", "image/x-icon"}, {
    0, 0}};

static const char *default_mimetype = "application/octet-stream";

/*
 * Determine MIME type from file extension
 */
static const char *
getmimetype(char *url)
{
    char *ext = strrchr(url, '.');


    if (ext) {
        int             i;

        ext++;
        for (i = 0; mimetab[i].name; ++i) {
            if (!strcmp(mimetab[i].name, ext)) {
                return mimetab[i].type;
            }
        }
    }
    return default_mimetype;
}


/*
 *   timerfc function Copyright 1996, Michiel Boland.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   1. Redistributions of source code must retain the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials
 *      provided with the distribution.
 *
 *   3. The name of the author may not be used to endorse or promote
 *      products derived from this software without specific prior
 *      written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY
 *   EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *   THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR
 *   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 *   IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *   THE POSSIBILITY OF SUCH DAMAGE.
 */
static          time_t
timerfc(const char *s)
{
    static const int daytab[2][12] = {
        {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334},
        {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335}
    };
    unsigned        sec,
                    min,
                    hour,
                    day,
                    mon,
                    year;
    char            month[3];
    int             c;
    unsigned        n;
    char            flag;
    char            state;
    char            isctime;
    enum { D_START, D_END, D_MON, D_DAY, D_YEAR, D_HOUR, D_MIN, D_SEC };

    sec = 60;
    min = 60;
    hour = 24;
    day = 32;
    year = 1969;
    isctime = 0;
    month[0] = 0;
    state = D_START;
    n = 0;
    flag = 1;
    do {
        c = *s++;
        switch (state) {
            case D_START:
                if (c == ' ') {
                    state = D_MON;
                    isctime = 1;
                } else if (c == ',')
                    state = D_DAY;
                break;
            case D_MON:
                if (isalpha(c)) {
                    if (n < 3)
                        month[n++] = c;
                } else {
                    if (n < 3)
                        return -1;
                    n = 0;
                    state = isctime ? D_DAY : D_YEAR;
                }
                break;
            case D_DAY:
                if (c == ' ' && flag);
                else if (isdigit(c)) {
                    flag = 0;
                    n = 10 * n + (c - '0');
                } else {
                    day = n;
                    n = 0;
                    state = isctime ? D_HOUR : D_MON;
                }
                break;
            case D_YEAR:
                if (isdigit(c))
                    n = 10 * n + (c - '0');
                else {
                    year = n;
                    n = 0;
                    state = isctime ? D_END : D_HOUR;
                }
                break;
            case D_HOUR:
                if (isdigit(c))
                    n = 10 * n + (c - '0');
                else {
                    hour = n;
                    n = 0;
                    state = D_MIN;
                }
                break;
            case D_MIN:
                if (isdigit(c))
                    n = 10 * n + (c - '0');
                else {
                    min = n;
                    n = 0;
                    state = D_SEC;
                }
                break;
            case D_SEC:
                if (isdigit(c))
                    n = 10 * n + (c - '0');
                else {
                    sec = n;
                    n = 0;
                    state = isctime ? D_YEAR : D_END;
                }
                break;
        }
    } while (state != D_END && c);
    switch (month[0]) {
        case 'A':
            mon = (month[1] == 'p') ? 4 : 8;
            break;
        case 'D':
            mon = 12;
            break;
        case 'F':
            mon = 2;
            break;
        case 'J':
            mon = (month[1] == 'a') ? 1 : ((month[2] == 'l') ? 7 : 6);
            break;
        case 'M':
            mon = (month[2] == 'r') ? 3 : 5;
            break;
        case 'N':
            mon = 11;
            break;
        case 'O':
            mon = 10;
            break;
        case 'S':
            mon = 9;
            break;
        default:
            return -1;
    }
    if (year <= 100)
        year += (year < 70) ? 2000 : 1900;
    --mon;
    --day;
    if (sec >= 60 || min >= 60 || hour >= 60 || day >= 31)
        return -1;
    if (year < 1970)
        return 0;
    return sec + 60L * (min + 60L * (hour + 24L * (day +
                                                   daytab[year % 4 == 0
                                                          && (year % 100
                                                              || year %
                                                              400 ==
                                                              0)][mon] +
                                                   365L * (year - 1970L) +
                                                   ((year -
                                                     1969L) >> 2))));
}

void
get_ucspi_env()
{
    char           *ucspi = getenv("PROTO");

    if (ucspi) {
        int             protolen = strlen(ucspi);
        char            buf[80];

        if (protolen > 20) {
            return;
        }
        strcpy(buf, ucspi);

        strcpy(buf + protolen, "REMOTEIP");
        remote_ip = getenv(buf);

        strcpy(buf + protolen, "REMOTEPORT");
        remote_port = getenv(buf);

        strcpy(buf + protolen, "REMOTEINFO");
        remote_ident = getenv(buf);
    }
}

void
parse_options(int argc, char *argv[])
{
    int             opt;

    while (-1 != (opt = getopt(argc, argv, "acdhkprv"))) {
        switch (opt) {
            case 'a':
                doauth = 1;
                break;
            case 'c':
                docgi = 1;
                break;
            case 'd':
                dirlist = 1;
                break;
            case 'p':
                portappend = 1;
                break;
            case 'r':
                redirect = 1;
                break;
            case 'v':
                printf(FNORD "\n");
                exit(0);
            default:
                fprintf(stderr, "Usage: %s [OPTIONS]\n",
                        argv[0]);
                fprintf(stderr, "\n");
                fprintf(stderr, "-a         Enable authentication\n");
                fprintf(stderr, "-c         Enable CGI\n");
                fprintf(stderr, "-d         Enable directory listing\n");
                fprintf(stderr, "-p         Append port to hostname directory\n");
                fprintf(stderr, "-r         Enable symlink redirection\n");
                fprintf(stderr, "-v         Print version and exit\n");
                exit(69);
        }
    }
}

enum { GET, POST, HEAD };

void
handle_request()
{
    char  request[MAXREQUESTLEN];
    char  fspath[MAXREQUESTLEN];
    char  buf[MAXHEADERLEN];
    char *p;
    char *query_string = NULL;
    int   method;
    time_t ims = 0;

    host = NULL;
    user_agent = NULL;
    refer = NULL;
    path = NULL;

    alarm(READTIMEOUT);

    /* Read request line first */
    request[0] = 0;
    fgets(request, sizeof request, stdin);
    if (!strncmp(request, "GET /", 5)) {
        method = GET;
        p = request + 5;
    } else if (!strncmp(request, "POST /", 6)) {
        method = POST;
        p = request + 6;
    } else if (!strncmp(request, "HEAD /", 6)) {
        method = HEAD;
        p = request + 6;
    } else {
        /* This also handles the case where fgets does nothing */
        badrequest(405, "Method Not Allowed", "Unsupported HTTP method.");
    }

    /* Interpret path into fspath */
    path = p - 1;
    {
        char *fsp = fspath;

        *(fsp++) = '/';
        for (; *p != ' '; p += 1) {
            if (! query_string) {
                char c = *p;

                switch (c) {
                    case 0:
                        badrequest(413, "Request Entity Too Large", "The HTTP request was too long");
                    case '\n':
                        badrequest(505, "Version Not Supported", "HTTP/0.9 not supported");
                    case '?':
                        query_string = p + 1;
                        break;
                    case '.':
                        if (p[-1] == '/') {
                            c = ':';
                        }
                        break;
                    case '%':
                        if (p[1] && p[2]) {
                            int a = fromhex(p[1]);
                            int b = fromhex(p[2]);

                            if ((a >= 0) && (b >= 0)) {
                                c = (a << 4) | b;
                                p += 2;
                            }
                        }
                        break;
                }

                *(fsp++) = c;
            }
        }
        *fsp = 0;
        DUMP_s(fspath);
    }
    *(p++) = 0;         /* NULL-terminate path */
    DUMP_s(path);

    http_version = -1;
    if (! strncmp(p, "HTTP/1.", 7) && (p[8] || (p[8] == '\r') || (p[8] == '\n'))) {
        http_version = p[7] - '0';
    }
    if (! ((http_version == 0) || (http_version == 1))) {
        badrequest(505, "Version Not Supported", "HTTP version not supported");
    }
    if (http_version == 1) {
        keepalive = 1;
    } else {
        keepalive = 0;
    }

    /* Read header fields */
    {
        char  *base = buf;
        int    nheaders = 0;

        while (1) {
            char   *cgi_name = base;
            char   *p;
            int     plen = (sizeof buf) - (base - buf);
            char   *name, *val;
            size_t  len;

            /* 40 is totally arbitrary here. */
            if (plen < 40) {
                badrequest(431, "Request Header Too Large", "The HTTP header block was too large");
            }
            if (nheaders++ >= MAXHEADERFIELDS) {
                badrequest(431, "Request Header Too Large", "Too many HTTP Headers");
            }
            strcpy(cgi_name, "HTTP_");
            plen -= 5;
            p = cgi_name + 5;

            if (NULL == fgets(p, plen, stdin)) {
                badrequest(500, "OS Error", "OS error reading headers");
            }

            len = extract_header_field(p, &val, 1);
            if (! len) {
                /* blank line */
                break;
            }

            name = p;
            if (! val) {
                badrequest(400, "Invalid header", "Unable to parse header block");
            }

            /* Set up CGI environment variables */
            setenv(cgi_name, val, 1);

            /* Handle special header fields */
            base = name + len + 1;
            if (! strcmp(name, "HOST")) {
                host = val;
            } else if (! strcmp(name, "USER_AGENT")) {
                user_agent = val;
            } else if (! strcmp(name, "REFERER")) {
                refer = val;
            } else if (! strcmp(name, "CONNECTION")) {
                if (! strcasecmp(val, "keep-alive")) {
                    keepalive = 1;
                } else {
                    keepalive = 0;
                }
            } else if (! strcmp(name, "IF_MODIFIED_SINCE")) {
                ims = timerfc(val);
            } else {
                /* We can re-use this buffer space */
                base = cgi_name;
            }
                
        }
    }

    /* Try to change into the appropriate directory */
    {
        char fn[PATH_MAX];

        strncpy(fn, host, sizeof fn);
        if (fn[0] == '.') {
            fn[0] = ':';
        }
        for (p = fn; *p; p += 1) {
            switch (*p) {
                case '/':
                    *p = ':';
                    break;
                case ':':
                    *p = 0;
                    break;
            }
        }

        if ((-1 == chdir(fn)) && (-1 == chdir("default"))) {
            badrequest(404, "Not Found", "This host is not served here");
        }
    }

    /* Serve the file */
    execl("/bin/sh", "sh", "-c", "set", NULL);

    return;
}

int
main(int argc, char *argv[], const char *const *envp)
{
    int cwd = open(".", O_RDONLY);
    parse_options(argc, argv);

    setbuffer(stdout, stdout_buf, sizeof stdout_buf);

    signal(SIGPIPE, SIG_IGN);
    get_ucspi_env();

    while (1) {
        handle_request();
        if (! keepalive) {
            break;
        }
        fchdir(cwd);
    }

    return 0;
}
