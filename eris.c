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
#include <sys/poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <dirent.h>
#include <sys/mman.h>
#include <limits.h>

/*
 * Some things I use for debugging 
 */
#define XXNODUMP

#ifndef NODUMP
#define DUMPf(fmt, args...) fprintf(stderr, "%s:%s:%d " fmt "\n", __FILE__, __FUNCTION__, __LINE__, ##args)
#else
#define DUMPf(fmt, args...)
#endif
#define DUMP() DUMPf("")
#define DUMP_d(v) DUMPf("%s = %d", #v, v)
#define DUMP_x(v) DUMPf("%s = 0x%x", #v, v)
#define DUMP_s(v) DUMPf("%s = %s", #v, v)
#define DUMP_c(v) DUMPf("%s = %c", #v, v)
#define DUMP_p(v) DUMPf("%s = %p", #v, v)
#define DUMP_buf(v, l) DUMPf("%s = %.*s", #v, l, v)

/*
 * the following is the time in seconds that fnord should wait for a valid 
 * HTTP request 
 */
#define READTIMEOUT 20

/*
 * the following is the time in seconds that fnord should wait before
 * aborting a request when trying to write the answer 
 */
#define WRITETIMEOUT 20

#define CGI_TIMEOUT	(5*60)  /* 5 minutes time-out for CGI to complete */

/*
 * defining USE_SENDFILE enables zero-copy TCP on Linux for static files.
 * I measured over 320 meg per second with apache bench over localhost
 * with sendfile and keep-alive.  However, sendfile does not work with
 * large files and may be considered cheating ;-) Also, sendfile is a
 * blocking operation.  Thus, no timeout handling. 
 */
#define USE_SENDFILE

#ifndef __linux__
#undef USE_SENDFILE
#endif

#ifdef USE_SENDFILE
#include <sys/sendfile.h>
#endif

#define USE_MMAP
#ifndef _POSIX_MAPPED_FILES
#undef USE_MMAP
#endif

enum { UNKNOWN, GET, HEAD, POST } method;

#ifdef TCP_CORK
static int      corked;
#endif
static long     retcode = 404;  /* used for logging code */
char           *host = "?";     /* Host: header */
char           *port;           /* also Host: header, :80 part */
char           *args;           /* URL behind ? (for CGIs) */
char           *url;            /* string between GET and HTTP/1.0, *
                                 * demangled */
char           *ua = "?";       /* user-agent */
char           *refer;          /* Referrer: header */
char           *accept_enc;     /* Accept-Encoding */
int             httpversion;    /* 0 == 1.0, 1 == 1.1 */
int             keepalive = 0;  /* should we keep the connection alive? */
int             rootdir;        /* fd of root directory, so we can fchdir
                                 * * back for keep-alive */
char           *cookie;         /* Referrer: header */
char           *uri;            /* copy of url before demangling */
char           *content_type;
char           *content_len;
char           *auth_type;
char           *post_miss;
unsigned long   post_mlen;
unsigned long   post_len = 0;

#if _FILE_OFFSET_BITS == 64
static unsigned long long rangestart,
                rangeend;       /* for ranged queries */
#define strtorange strtoull
#else
static unsigned long rangestart,
                rangeend;       /* for ranged queries */
#define strtorange strtoul
#endif

static const char days[] = "SunMonTueWedThuFriSat";
static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

#define MAXHEADERLEN 8192

char           *remote_ip;
char           *remote_port;
char           *remote_ident;

#define BUFFER_OUTSIZE 8192
char stdout_buf[BUFFER_OUTSIZE];

static void
sanitize(char *ua)
{                               /* replace strings with underscores for *
                                 * logging */
    int             j;
    if (!ua)
        return;
    for (j = 0; ua[j]; ++j)
        if (isspace(ua[j]))
            ua[j] = '_';
}

static void
dolog(off_t len)
{                               /* write a log line to stderr */
    sanitize(host);
    sanitize(ua);
    sanitize(refer);

    fprintf(stderr, "%s %ld %lu %s %s %s %s\n",
            remote_ip ? remote_ip : "0.0.0.0",
            retcode, (unsigned long) len, host, ua, refer, url);
}

/*
 * output an error message and exit 
 */
static void
badrequest(long code, const char *httpcomment, const char *message)
{
    retcode = code;
    dolog(0);
    printf("HTTP/1.0 %ld %s\r\nConnection: close\r\n", code, httpcomment);
    if (message && message[0]) {
        printf("Content-Length: %lu\r\nContent-Type: text/html\r\n\r\n",
               (unsigned long) strlen(message));
        fputs(message, stdout);
    } else {
        fputs("\r\n", stdout);
    }
    fflush(stdout);
    exit(0);
}

#define CGIENVLEN 21

static const char *cgivars[CGIENVLEN] = {
    "GATEWAY_INTERFACE=",
    "SERVER_PROTOCOL=",
    "SERVER_SOFTWARE=",
    "SERVER_NAME=",
    "SERVER_PORT=",
    "REQUEST_METHOD=",
    "REQUEST_URI=",
    "SCRIPT_NAME=",
    "REMOTE_ADDR=",
    "REMOTE_PORT=",
    "REMOTE_IDENT=",
    "HTTP_USER_AGENT=",
    "HTTP_COOKIE=",
    "HTTP_REFERER=",
    "HTTP_ACCEPT_ENCODING=",
    "AUTH_TYPE=",
    "CONTENT_TYPE=",
    "CONTENT_LENGTH=",
    "QUERY_STRING=",
    "PATH_INFO=",
    "PATH_TRANSLATED="
};

static int
iscgivar(const char *s)
{
    int             sl = strlen(s);
    register unsigned int i = 0;

    for (; i < CGIENVLEN; i++)
        if (!strncmp(s, cgivars[i], sl))
            return 1;
    return 0;
}

static unsigned int
elen(register const char *const *e)
{
    register unsigned int i = 0;
    while (e[i])
        i++;
    return i;
}

static ssize_t
read_header(int fd, char *buf, size_t buflen)
{
    size_t len = 0;
    int found = 0;
    size_t p = 0;

    while (found < 2) {
        int             tmp;

        tmp = read(fd, buf + len, buflen - len);
        if (tmp < 0) {
            return -1;
        }
        if (tmp == 0) {
            break;
        }
        len += tmp;

        for (; p < len; p += 1) {
            if (buf[p] == '\n') {
                if (++found == 2) {
                    break;
                }
            }
        }
    }

    return len;
}

char *
env_append(const char *key, const char *val)
{
    static char     buf[MAXHEADERLEN * 2 + PATH_MAX + 200];
    static char    *p = buf;
    char           *ret = p;

    if (!key) {
        p = buf;
        return NULL;
    }

    p = stpcpy(p, key);
    *(p++) = '=';
    if (val) {
        p = stpcpy(p, val) + 1;
    } else {
        *(p++) = 0;
    }

    return ret;
}

static void
do_cgi(const char *pathinfo, const char *const *envp)
{
    const char     *method_name[] = { "?", "GET", "HEAD", "POST" };
    register unsigned int en = elen(envp);
    char          **cgi_arg;
    register int    i;
    char          **cgi_env =
        (char **) alloca((CGIENVLEN + en + 1) * sizeof(char *));

    cgi_env[0] = "GATEWAY_INTERFACE=CGI/1.1";
    cgi_env[1] = "SERVER_PROTOCOL=HTTP/1.0";
    cgi_env[2] = "SERVER_SOFTWARE=" FNORD;

    i = 3;
    env_append(NULL, NULL);
    cgi_env[i++] = env_append("SERVER_NAME", host);
    cgi_env[i++] = env_append("SERVER_PORT", port);
    cgi_env[i++] = env_append("REQUEST_METHOD", method_name[method]);
    cgi_env[i++] = env_append("REQUEST_URI", uri);
    cgi_env[i++] = env_append("SCRIPT_NAME", url);
    if (remote_ip)
        cgi_env[i++] = env_append("REMOTE_ADDR", remote_ip);
    if (remote_port)
        cgi_env[i++] = env_append("REMOTE_PORT", remote_port);
    if (remote_ident)
        cgi_env[i++] = env_append("REMOTE_IDENT", remote_ident);
    if (ua)
        cgi_env[i++] = env_append("HTTP_USER_AGENT", ua);
    if (cookie)
        cgi_env[i++] = env_append("HTTP_COOKIE", cookie);
    if (refer)
        cgi_env[i++] = env_append("HTTP_REFERER", refer);
    if (accept_enc)
        cgi_env[i++] = env_append("HTTP_ACCEPT_ENCODING", accept_enc);
    if (auth_type)
        cgi_env[i++] = env_append("AUTH_TYPE", auth_type);
    if (content_type)
        cgi_env[i++] = env_append("CONTENT_TYPE", content_type);
    if (content_type)
        cgi_env[i++] = env_append("CONTENT_LENGTH", content_len);
    if (args)
        cgi_env[i++] = env_append("QUERY_STRING", args);
    if (pathinfo) {
        char           *rp = realpath(pathinfo, NULL);

        cgi_env[i++] = env_append("PATH_INFO", pathinfo);
        cgi_env[i++] = env_append("PATH_TRANSLATED", rp ? rp : pathinfo);
        if (rp)
            free(rp);
    }

    {
        unsigned int    j = 0;
        for (; j < en; j++)
            if (!iscgivar(envp[j]))
                cgi_env[++i] = (char *) envp[j];
    }
    cgi_env[++i] = 0;

    /*
     * argv 
     */
    if (args && (strchr(args, '=') == 0)) {
        int             n = 3;
        for (i = 0; args[i]; ++i)
            if (args[i] == '+')
                ++n;
        cgi_arg = alloca(n * sizeof(char *));
        cgi_arg[n = 1] = args;
        for (i = 0; args[i]; ++i) {
            if (args[i] == '+') {
                args[i] = 0;
                ++i;
                cgi_arg[++n] = args + i;
            }
        }
        cgi_arg[++n] = 0;
    } else {
        cgi_arg = alloca(2 * sizeof(char *));
        cgi_arg[1] = 0;
    }

    {
        char            tmp[PATH_MAX];

        i = strrchr(url, '/') - url;
        strncpy(tmp, url + 1, i);
        tmp[i] = 0;
        chdir(tmp);
    }

    {
        char            tmp[PATH_MAX];

        /*
         * program name 
         */
        cgi_arg[0] = tmp;
        tmp[0] = '.';
        strcpy(tmp + 1, url + i);

        /*
         * start cgi 
         */
        execve(cgi_arg[0], cgi_arg, cgi_env);
        raise(SIGQUIT);         /* gateway unavailable. */
    }
}

static void
cgi_child(int sig)
{
    int             n,
                    pid = waitpid(0, &n, WNOHANG);
    if (pid > 0) {
        if (WIFSIGNALED(n)) {
            if (WTERMSIG(n) == SIGALRM)
                badrequest(504, "Gateway Time-out",
                           "Gateway has hit the Time-out.");
            else
                badrequest(502, "Bad Gateway",
                           "Gateway broken or unavailable.");
        }
    }
    signal(SIGCHLD, cgi_child);
}

/* Convert bare \n to \r\n in header.  Return 0 if
 * header is over. */
static int
cgi_send_correct_http(const char *s, unsigned int sl)
{
    unsigned int    i;
    int newline = 0;

    for (i = 0; i < sl; i += 1) {
        switch (s[i]) {
            case '\r':
                if (s[i + 1] == '\n') {
                    i += 1;
            case '\n': 
                    printf("\r\n");
                    if (newline) {
                        fwrite(s + i + 1, sl - i - 1, 1, stdout);
                        return 0;
                    } else {
                        newline = 1;
                    }
                    break;
                } else {
            default:
                    newline = 0;
                    putchar(s[i]);
                }
                break;
        }
    }
    return 1;
}

static void
start_cgi(int nph, const char *pathinfo, const char *const *envp)
{
    size_t          size = 0;
    int             n;
    int             pid;
    char            ibuf[8192],
                    obuf[8192];
    int             fd[2],
                    df[2];

    if (pipe(fd) || pipe(df)) {
        badrequest(500, "Internal Server Error",
                   "Server Resource problem.");
    }

    if ((pid = fork())) {
        if (pid > 0) {
            struct pollfd   pfd[2];
            int             nr = 1;
            int             startup = 1;

            signal(SIGCHLD, cgi_child);
            signal(SIGPIPE, SIG_IGN);   /* NO! no signal! */

            close(df[0]);
            close(fd[1]);

            pfd[0].fd = fd[0];
            pfd[0].events = POLLIN;
            pfd[0].revents = 0;

            pfd[1].fd = df[1];
            pfd[1].events = POLLOUT;
            pfd[1].revents = 0;

            if (post_len)
                ++nr;           /* have post data */
            else
                close(df[1]);   /* no post data */

            while (poll(pfd, nr, -1) != -1) {
                /*
                 * read from cgi 
                 */
                if (pfd[0].revents & POLLIN) {
                    size_t len;

                    if (startup) {
                        /* XXX: could block :< */
                        len = read_header(fd[0], ibuf, sizeof ibuf);
                    } else {
                        len = read(fd[0], ibuf, sizeof ibuf);
                    }
                    
                    if (0 == len) {
                        break;
                    }
                    if (len == -1) {
                        goto cgi_500;
                    }

                    /*
                     * startup 
                     */
                    if (startup) {
                        if (nph) {      /* NPH-CGI */
                            startup = 0;
                            printf("%.*s", len, ibuf);
                            /*
                             * skip HTTP/x.x 
                             */
                            retcode = strtoul(ibuf + 9, NULL, 10);
                        } else {        /* CGI */
                            if (memcmp(ibuf, "Location: ", 10) == 0) {
                                retcode = 302;
                                printf
                                    ("HTTP/1.0 302 CGI-Redirect\r\nConnection: close\r\n");
                                signal(SIGCHLD, SIG_IGN);
                                cgi_send_correct_http(ibuf, n);
                                fflush(stdout);
                                dolog(0);
                                exit(0);
                            } else {
                                retcode = 200;
                                printf("HTTP/1.0 200 OK\r\nServer: "
                                       FNORD
                                       "\r\nPragma: no-cache\r\nConnection: close\r\n");
                                signal(SIGCHLD, SIG_IGN);
                                cgi_send_correct_http(ibuf, len);
                                startup = 0;
                            }
                        }
                    }
                    /*
                     * non startup 
                     */
                    else {
                        fwrite(ibuf, len, 1, stdout);
                    }
                    size += len;
                    if (pfd[0].revents & POLLHUP)
                        break;
                }
                /*
                 * write to cgi the post data 
                 */
                else if (nr > 1 && pfd[1].revents & POLLOUT) {
                    if (post_miss) {
                        write(df[1], post_miss, post_mlen);
                        post_miss = 0;
                    } else if (post_mlen < post_len) {
                        n = read(0, obuf, sizeof(obuf));
                        if (n < 1)
                            goto cgi_500;
                        post_mlen += n;
                        write(df[1], obuf, n);
                    } else {
                        --nr;
                        close(df[1]);
                    }
                } else if (pfd[0].revents & POLLHUP)
                    break;
                else {
                  cgi_500:if (startup)
                        badrequest(500, "Internal Server Error",
                                   "Looks like the CGI crashed.");
                    else {
                        printf("\n\nLooks like the CGI crashed.\n\n");
                        break;
                    }
                }
            }

            fflush(stdout);
            dolog(size);
#ifdef TCP_CORK
            {
                int             zero = 0;
                setsockopt(1, IPPROTO_TCP, TCP_CORK, &zero, sizeof(zero));
            }
#endif
        }
    } else {
        close(df[1]);
        close(fd[0]);

        dup2(df[0], 0);
        dup2(fd[1], 1);

        close(df[0]);
        close(fd[1]);

        alarm(CGI_TIMEOUT);
        do_cgi(pathinfo, envp);
    }
    exit(0);
}

static int
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

static char    *encoding = 0;
static char    *mimetype = "application/octet-stream";

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
0}};

/*
 * try to find out MIME type and content encoding. This is called twice,
 * once for the actual URL and once for URL.gz. If the actual URL already
 * ende with .gz, return application/octet-stream to make sure the client
 * can download the file even if he does not support gzip encoding 
 */
static void
getmimetype(char *url, int explicit)
{
    char            save;
    int             ext;
    ext = strlen(url);
    while (ext > 0 && url[ext] != '.' && url[ext] != '/')
        --ext;
    if (url[ext] == '.') {
        ++ext;
        if (!strcmp(url + ext, "bz2"))
            goto octetstream;
        if (!strcmp(url + ext, "gz")) {
            if (!encoding) {
                if (explicit)
                    goto octetstream;
                encoding = "gzip";
                save = url[ext - 1];
                url[ext - 1] = 0;
                getmimetype(url, explicit);
                url[ext - 1] = save;
            } else
              octetstream:
                mimetype = "application/octet-stream";
        } else {
            int             i;
            for (i = 0; mimetab[i].name; ++i)
                if (!strcmp(mimetab[i].name, url + ext)) {
                    mimetype = (char *) mimetab[i].type;
                    break;
                }
        }
    }
}

static int
matchcommalist(const char *needle, const char *haystack)
{
    /*
     * needle: "text/html", haystack: the accept header, "text/html,
     * text/plain\r\n" 
     */
    /*
     * return nonzero if match was found 
     */
    int             len = strlen(needle);
    if (strncmp(needle, haystack, len))
        return 0;
    switch (haystack[len]) {
    case ';':
    case ',':
    case '\r':
    case '\n':
    case 0:
        return 1;
    }
    return 0;
}

static int
findincommalist(const char *needle, const char *haystack)
{
    const char     *accept;
    for (accept = haystack; accept;) {
        /*
         * format: foo/bar, 
         */
        const char     *tmp = accept;
        int             final;
        while (*tmp) {
            if (*tmp == ';')
                break;
            else if (*tmp == ',')
                break;
            ++tmp;
        }
        final = (*tmp == 0 || *tmp == ';');
        if (matchcommalist("*/*", accept))
            break;
        if (matchcommalist(haystack, accept))
            break;
        accept = tmp + 1;
        if (final)
            return 0;
    }
    return 1;
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

static struct stat st;

/*
 * try to return a file 
 */
static int
doit(char *buf, int buflen, char *url, int explicit)
{
    int             fd = -1;
    char           *accept;
    while (url[0] == '/')
        ++url;
    getmimetype(url, explicit);
    {
        char           *b = buf;
        int             l = buflen;
        for (;;) {
            char           *h = header(b, l, "Accept");
            if (!h)
                goto ok;
            if (findincommalist(mimetype, h))
                goto ok;
            l -= (h - b) + 1;
            b = h + 1;
        }
        retcode = 406;
        goto bad;
    }
  ok:
    if (encoding) {             /* see if client accepts the encoding */
        char           *tmp = header(buf, buflen, "Accept-Encoding");
        if (!tmp || !strstr(tmp, "gzip")) {
            retcode = 406;
            goto bad;
        }
    }
    if ((fd = open(url, O_RDONLY)) >= 0) {
        if (fstat(fd, &st))
            goto bad;
        /*
         * no directories 
         */
        if (S_ISDIR(st.st_mode))
            goto bad;
        /*
         * see if the peer accepts MIME type 
         */
        /*
         * see if the document has been changed 
         */
        {
            char           *field =
                header(buf, buflen, "If-Modified-Since");

            if (field) {
                time_t          ims;

                ims = timerfc(field);
                if ((ims != (time_t) - 1) && (st.st_mtime <= ims)) {
                    retcode = 304;
                    goto bad;
                }
            }
        }
        rangestart = 0;
        rangeend = st.st_size;
        if ((accept = header(buf, buflen, "Range"))) {
            /*
             * format: "bytes=17-23", "bytes=23-" 
             */
            if (!strncmp(accept, "bytes=", 6)) {
                accept += 6;
                rangestart = strtorange(accept, &accept, 10);
                if (*accept == '-') {
                    ++accept;
                    if (*accept) {
                        rangeend = strtorange(accept, &accept, 10);
                        if (!*accept) {
                            rangeend = st.st_size;
                        } else {
                            ++rangeend;
                        }
                    }
                }
            }
            if (rangestart > rangeend || rangeend > st.st_size) {
                retcode = 416;
                goto bad;
            }
        }
        return fd;
      bad:
        if (fd >= 0)
            close(fd);
    }
    return -1;
}

static void
redirectboilerplate()
{
    printf
        ("HTTP/1.0 301 Go Away\r\nConnection: close\r\nContent-Length: 0\r\nLocation: ");
}

static void
handleredirect(const char *url, const char *origurl)
{
    char            symlink[1024];
    int             len;

    while (*url == '/')
        ++url;
    if ((len = readlink(url, symlink, 1023)) > 0) {
        /*
         * el-cheapo redirection 
         */
        redirectboilerplate();
        printf("%.*s", len, symlink);
        retcode = 301;
        printf("\r\n\r\n");
        dolog(0);
        fflush(stdout);
        exit(0);
    }
}

static void
hdl_encode_html(const char *s, unsigned int sl)
{
    int             i;
    for (i = 0; i < sl; ++i) {
        unsigned char   ch = s[i];
        if (ch > 159) {
            printf("&#%u;", ch);
        } else if ((ch > 128) || (ch < 32)) {
            putchar('_');
        } else if (ch == '"')
            fputs("&quot;", stdout);
        else if (ch == '&')
            fputs("&amp;", stdout);
        else if (ch == '<')
            fputs("&lt;", stdout);
        else if (ch == '>')
            fputs("&gt;", stdout);
        else
            putchar(ch);
    }
}
static void
hdl_encode_uri(const char *s, unsigned int sl)
{
    int             i;
    for (i = 0; i < sl; ++i) {
        unsigned char   ch = s[i];
        if ((ch != '%') && (ch > 32) && (ch < 127))
            putchar(ch);
        else
            printf("%%%02x", ch);
    }
}
static void
handledirlist(const char *origurl)
{
    DIR            *dir;
    unsigned int    nl;
    const char     *nurl = origurl;

    url = (char *) origurl;
    while (nurl[0] == '/')
        ++nurl;
    if (nurl <= origurl)
        return;
    nl = strlen(nurl);
    if (nl && (nurl[nl - 1] != '/'))
        return;
    if (!stat(nl ? nurl : ".", &st) && (S_ISDIR(st.st_mode))
        && ((st.st_mode & S_IRWXO) == 5)) {
        if (nl)
            chdir(nurl);
        if ((dir = opendir("."))) {
            struct dirent  *de;
            unsigned int    i,
                            size = 32 + nl;
            fputs("HTTP/1.0 200 OK\r\nServer: " FNORD
                  "\r\nConnection: close\r\n", stdout);
            fputs("Content-Type: text/html; charset=utf-8\r\n", stdout);
            fputs("\r\n<h3>Directory Listing: /", stdout);
            hdl_encode_html(nurl, nl);
            fputs("</h3>\n<pre>\n", stdout);
            if (nl != 0) {
                for (i = nl - 2; i > 0; --i)
                    if (nurl[i] == '/')
                        break;
                fputs("<a href=\"", stdout);
                fputs("/", stdout);
                hdl_encode_uri(nurl, i);
                if (i > 0)
                    fputs("/", stdout);
                fputs("\">Parent directory", stdout);
                fputs("</a>\n", stdout);
                size += 40 + i;
            }
            while ((de = readdir(dir))) {
                char            symlink[1024];
                char           *p = de->d_name;
                unsigned int    pl,
                                dl = strlen(de->d_name);
                pl = dl;
                if (de->d_name[0] == '.')
                    continue;   /* hidden files -> skip */
                if (lstat(de->d_name, &st))
                    continue;   /* can't stat -> skip */
                if (S_ISDIR(st.st_mode))
                    fputs("[DIR] ", stdout);
                else if (S_ISLNK(st.st_mode)) {
                    if ((pl = readlink(de->d_name, symlink, 1023)) < 1)
                        continue;
                    p = symlink;
                    fputs("[LNK] ", stdout);    /* a symlink to *
                                                 * something ... */
                } else if (S_ISREG(st.st_mode))
                    fputs("[TXT] ", stdout);
                else
                    continue;   /* not a file we can provide -> skip */
                /*
                 * write a href 
                 */
                fputs("<a href=\"", stdout);
                hdl_encode_uri(p, pl);
                if (S_ISDIR(st.st_mode))
                    fputs("/", stdout), ++size;
                fputs("\">", stdout);
                if (de->d_name[0] == ':')
                    de->d_name[0] = '.';        /* fnord special ... */
                hdl_encode_html(de->d_name, dl);
                fputs("</a>\n", stdout);
                size += 22 + (dl << 1);
            }
            closedir(dir);
            fputs("</pre>\n", stdout);
            fflush(stdout);
            retcode = 200;
            dolog(size);
            exit(0);
        }
    }
}

static int
handleindexcgi(const char *testurl, const char *origurl, char *space)
{
    unsigned int    ul,
                    ol = strlen(origurl);
    char           *test;
    while (testurl[0] == '/')
        ++testurl, --ol;
    ul = strlen(testurl);
    if (strcmp(testurl + ol, "index.html"))
        return 0;               /* no request for index.html */
    test = space;
    ++test;
    ul -= 4;
    memcpy(test, testurl, ul);
    test[ul] = 'c';
    test[++ul] = 'g';
    test[++ul] = 'i';
    test[++ul] = 0;
    if (stat(test, &st))
        return 0;               /* no index.cgi present */
    ul = 1;
    if (st.st_gid == getegid())
        ul = 010;
    if (st.st_uid == geteuid())
        ul = 0100;
    if (!(st.st_mode & ul))
        return 0;               /* should be executable */
    *(--test) = '/';
    url = test;
    return 1;                   /* Wow... now start "index.cgi" */
}

static void
get_ucspi_env(void)
{
    char           *ucspi = getenv("PROTO");
    if (ucspi) {
        int             protolen = strlen(ucspi);
        char           *buf = alloca(protolen + 20);

        strcpy(buf, ucspi);

        strcpy(buf + protolen, "REMOTEIP");
        remote_ip = getenv(buf);

        strcpy(buf + protolen, "REMOTEPORT");
        remote_port = getenv(buf);

        strcpy(buf + protolen, "REMOTEINFO");
        remote_ident = getenv(buf);
    }
}

static int
findcgi(const char *c)
{
    return (c[0] == '.' && c[1] == 'c' &&
            c[2] == 'g' && c[3] == 'i' && (c[4] == '/' || c[4] == 0));
}

static int
serve_read_write(int fd)
{
    char            tmp[4096];
    struct pollfd   duh;
    time_t          now,
                    fini;
    char           *tmp2;
    int             len;
    off_t           todo = rangeend - rangestart;
    duh.fd = 1;
    duh.events = POLLOUT;
    if (rangestart)
        lseek(fd, rangestart, SEEK_SET);
    while (todo > 0) {
        int             olen;
        fini = time(&now) + WRITETIMEOUT;
        len = read(fd, tmp, todo > 4096 ? 4096 : todo);
        olen = len;
        tmp2 = tmp;
        while (len > 0) {
            int             written;
            switch (poll(&duh, 1, (fini - now) * 1000)) {
            case 0:
                if (now < fini)
                    continue;   /* fall through */
            case -1:
                return 1;       /* timeout or error */
            }
            if ((written = write(1, tmp2, len)) < 0)
                return -1;
            len -= written;
            tmp2 += written;
            time(&now);
        }
        todo -= olen;
    }
    return 0;
}

static int
serve_mmap(int fd)
{
    off_t           mapstart,
                    maplen;
    unsigned long   mapofs;
    char           *map,
                   *tmp2;
    struct pollfd   duh;
    time_t          now,
                    fini;
    mapstart = rangestart & (~(off_t) 0xfff);   /* round down to 4k page */
    maplen = rangeend - mapstart;
    mapofs = rangestart - mapstart;
    if (maplen > 64 * 1024 * 1024)
        maplen = 64 * 1024 * 1024;
    map = mmap(0, maplen, PROT_READ, MAP_PRIVATE, fd, mapstart);
    if (map == MAP_FAILED) {
        if (errno == EINVAL && mapstart) {
            /*
             * try rounded to 64k pages 
             */
            mapstart = rangestart & 0xffff;
            maplen = rangeend - mapstart;
            mapofs = rangestart - mapstart;
            map = mmap(0, maplen, PROT_READ, MAP_PRIVATE, fd, mapstart);
            if (map == MAP_FAILED)
                /*
                 * didn't work, use read/write instead. 
                 */
                return serve_read_write(fd);
        } else
            return serve_read_write(fd);
    }
    duh.fd = 1;
    duh.events = POLLOUT;
    while (rangestart < rangeend) {
        int             len;
        fini = time(&now) + WRITETIMEOUT;
        len = maplen - mapofs;
        tmp2 = map + mapofs;
        while (len > 0) {
            int             written;
            switch (poll(&duh, 1, (fini - now) * 1000)) {
            case 0:
                if (now < fini)
                    continue;   /* fall through */
            case -1:
                return 1;       /* timeout or error */
            }
            if ((written = write(1, tmp2, len)) < 0)
                return -1;
            len -= written;
            tmp2 += written;
            time(&now);
        }
        rangestart += maplen - mapofs;
        mapstart += maplen;
        munmap(map, maplen);
        mapofs = 0;
        maplen = rangeend - mapstart;
        if (maplen) {
            if (maplen > 64 * 1024 * 1024)
                maplen = 64 * 1024 * 1024;
            map = mmap(0, maplen, PROT_READ, MAP_SHARED, fd, mapstart);
            if (map == MAP_FAILED)
                /*
                 * can't happen, really 
                 */
                return serve_read_write(fd);
        }
    }
    return 0;
}

/*
 * write from offset "rangestart" to offset "rangeend" to fd #1 
 */
static int
serve_static_data(int fd)
{
    off_t           len = rangeend - rangestart;
#ifdef TCP_CORK
    corked = 0;
#endif
    if (len < 4096) {           /* for small files, sendfile is actually
                                 * slower */
        char            tmp[4096];
        if (rangestart)
            lseek(fd, rangestart, SEEK_SET);
        read(fd, tmp, len);     /* if read fails, we can't back down now.
                                 * We already committed on the
                                 * content-length */
        fwrite(tmp, len, 1, stdout);
        fflush(stdout);
        return 0;
    }
#ifdef USE_SENDFILE
    {
        off_t           offset = rangestart;
#ifdef TCP_CORK
        {
            int             one = 1;
            setsockopt(1, IPPROTO_TCP, TCP_CORK, &one, sizeof(one));
            corked = 1;
        }
#endif
        fflush(stdout);
        {
            off_t           l = rangeend - rangestart;
            do {
                off_t           c;
                c = (l > (1ul << 31)) ? 1ul << 31 : l;
                if (sendfile(1, fd, &offset, c) == -1)
#ifdef USE_MMAP
                    return serve_mmap(fd);
#else
                    return serve_read_write(fd);
#endif
                l -= c;
            } while (l);
        }
        return 0;
    }
#else
    fflush(stdout);
#ifdef TCP_CORK
    {
        int             one = 1;
        setsockopt(1, IPPROTO_TCP, TCP_CORK, &one, sizeof(one));
        corked = 1;
    }
#endif
#ifdef USE_MMAP
    return serve_mmap(fd);
#else
    return serve_read_write(fd);
#endif
#endif
}


int
main(int argc, char *argv[], const char *const *envp)
{
    char            buf[MAXHEADERLEN];
    char           *nurl,
                   *origurl;
    int             docgi = 0;
    int             dirlist = 0;
    int             redirect = 0;
    int             portappend = 0;
    int             len;
    int             in;

    {
        int             opt;

        while (-1 != (opt = getopt(argc, argv, "cdrp"))) {
            switch (opt) {
            case 'c':
                docgi = 1;
                break;
            case 'd':
                dirlist = 1;
                break;
            case 'r':
                redirect = 1;
                break;
            case 'p':
                portappend = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-c] [-d] [-r] [-p]\n", argv[0]);
                return 69;
            }
        }
    }

    setbuffer(stdout, stdout_buf, sizeof stdout_buf);

    signal(SIGPIPE, SIG_IGN);
    get_ucspi_env();

  handlenext:
    encoding = 0;
    // alarm(20);

    {
        int             found = 0;
        time_t          fini,
                        now;
        struct pollfd   duh;

        fini = time(&now) + READTIMEOUT;
        duh.fd = 0;
        duh.events = POLLIN;
        for (in = len = 0; found < 2;) {
            int             tmp;
            switch (poll(&duh, 1, READTIMEOUT * 1000)) {
            case 0:
                if (time(&now) < fini)
                    continue;   /* fall through */
            case -1:           /* timeout or error */
                // badrequest(408,"Request Time-out","No request
                // appeared
                // within a reasonable time period.");
                return 1;
            }
            tmp = read(0, buf + len, MAXHEADERLEN - len - 5);
            if (tmp < 0)
                return 1;
            if (tmp == 0)
                return 1;
            in += tmp;
            now = time(0);
            for (; (found < 2) && (len < in); ++len) {
                if (buf[len] == '\r')
                    continue;
                if (buf[len] == '\n')
                    ++found;
                else
                    found = 0;
                if (found > 1)
                    break;
            }
        }
    }
    if (len < 10)
        badrequest(400, "Bad Request",
                   "<title>Bad Request</title>That does not look like HTTP to me...");
    buf[len] = 0;

    if (!strncasecmp(buf, "GET /", 5)) {
        method = GET;
        url = buf + 4;
    } else if (!strncasecmp(buf, "POST /", 6)) {
        method = POST;
        url = buf + 5;
    } else if (!strncasecmp(buf, "HEAD /", 6)) {
        method = HEAD;
        url = buf + 5;
    } else
        badrequest(400, "Bad Request",
                   "<title>Bad Request</title>Unsupported HTTP method.");

    origurl = url;

    {
        char           *nl = strchr(buf, '\r');
        char           *space = strchr(url, ' ');
        if (space >= nl)
            badrequest(400, "Bad Request",
                       "<title>Bad Request</title>HTTP/0.9 not supported");
        if (strncmp(space + 1, "HTTP/1.", 7))
            badrequest(400, "Bad Request",
                       "<title>Bad Request</title>Only HTTP 1.x supported");
        *space = 0;
        httpversion = space[8] - '0';
        keepalive = 0;

        /*
         * demangle path in-place 
         */
        {
            register char  *tmp,
                           *d;
            for (tmp = d = url; *tmp; ++tmp) {
                if (*tmp == '?') {
                    args = tmp + 1;
                    break;
                }
                if (*tmp == ' ')
                    break;
                if (*tmp == '%') {
                    int             a,
                                    b;
                    a = fromhex(tmp[1]);
                    b = fromhex(tmp[2]);
                    if (a >= 0 && b >= 0) {
                        *d = (a << 4) + b;
                        tmp += 2;
                    } else
                        *d = *tmp;
                } else
                    *d = *tmp;
                if (d > url + 1 && *d == '/' && d[-1] == ':'
                    && d[-2] == '/')
                    d -= 2;
                if (d > url && *d == '/' && d[-1] == '/')
                    --d;
                if (d > url && *d == '.' && d[-1] == '/')
                    *d = ':';
                ++d;
            }
            *d = 0;
            /*
             * not good enough, we need a second pass 
             */
        }

        uri = strdup(url);
    }

    {
        char           *tmp;
        ua = header(buf, len, "User-Agent");
        refer = header(buf, len, "Referer");
        accept_enc = header(buf, len, "Accept-Encoding");
        if ((tmp = header(buf, len, "Connection"))) {   /* see if it's
                                                         * "keep-alive"
                                                         * or * "close" */
            if (!strcasecmp(tmp, "keep-alive"))
                keepalive = 1;
            else if (!strcasecmp(tmp, "close"))
                keepalive = -1;
        }
        cookie = header(buf, len, "Cookie");
        auth_type = header(buf, len, "Authorization");
        if (method == POST) {
            content_type = header(buf, len, "Content-Type");
            content_len = header(buf, len, "Content-Length");
            if (content_len) {
                post_len = strtoul(content_len, NULL, 10);
                post_miss = buf + len + 1;
                post_mlen = in - len - 1;
                if (post_len <= post_mlen)
                    post_mlen = post_len;
            }
        }
    }

    port = getenv("TCPLOCALPORT");
    if (!port)
        port = "80";
    {
        char           *Buf;
        int             i;
        host = header(buf, len, "Host");
        if (!host)
            i = 100;
        else
            i = strlen(host) + 7;
        Buf = alloca(i);
        if (!host) {
            char           *ip = getenv("TCPLOCALIP");
            if (!ip)
                ip = "127.0.0.1";
            if (strlen(ip) + strlen(port) > 90)
                exit(101);
            if (portappend) {
                sprintf(Buf, "%s:%s", ip, port);
            } else {
                strcpy(Buf, ip);
            }
            host = Buf;
        } else {
            char           *colon = strchr(host, ':');

            if (portappend && !colon) {
                sprintf(Buf, "%s:%s", host, port);
                host = Buf;
            } else if (!portappend && colon) {
                *colon = '\0';
            }
        }
        for (i = strlen(host); i >= 0; --i)
            if ((host[i] = tolower(host[i])) == '/')
              hostb0rken:
                badrequest(400, "Bad Request",
                           "<title>Bad Request</title>Bullshit Host header");
        if (host[0] == '.')
            goto hostb0rken;
        // fprintf(stderr,"host %s\n",host);
        if (keepalive > 0) {
            if ((rootdir = open(".", O_RDONLY)) < 0)
                keepalive = -1;
        }
        if (chdir(host)) {
            if (redirect) {
                char            symlink[1024];
                int             linklen;
                if ((linklen =
                     readlink(host, symlink, sizeof symlink)) > 0) {
                    /*
                     * it is a broken symlink.  Do a redirection 
                     */
                    redirectboilerplate();
                    if (symlink[0] == '=') {
                        fwrite(symlink + 1, linklen - 1, 1, stdout);
                    } else {
                        fwrite(symlink, linklen, 1, stdout);
                        while (url[0] == '/')
                            ++url;
                        fputs(url, stdout);
                    }
                    retcode = 301;
                    fputs("\r\n\r\n", stdout);
                    dolog(0);
                    fflush(stdout);
                    exit(0);
                }
            }
            if (chdir("default") && argc < 2) {
                badrequest(404, "Not Found",
                           "<title>Not Found</title>This host is not served here.");
            }
        }
    }
#ifdef AUTH
    {
        char           *auth_script = ".http-auth";
        struct stat     st;

        if (!stat(auth_script, &st)) {
            pid_t           child;
            const char     *authorization;

            authorization = header(buf, len, "Authorization");
            child = fork();
            if (child < 0) {
                badrequest(500, "Internal Server Error",
                           "Server Resource problem.");
            } else if (child == 0) {
                const char     *argv[5] =
                    { auth_script, host, url, authorization, NULL };

                dup2(2, 1);
                execve(auth_script, argv, envp);
                _exit(1);
            } else {
                int             status;
                pid_t           childr;

                while ((childr = waitpid(child, &status, 0)) < 0
                       && errno == EINTR);
                if (childr != child)
                    badrequest(500, "Internal Server Error",
                               "Server system problem.");
                if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                    retcode = 401;
                    dolog(0);
                    fputs("HTTP/1.0 401 Authorization Required\r\n"
                          "WWW-Authenticate: Basic realm=\"", stdout);
                    fputs(host, stdout);
                    fputs("\"\r\nConnection: close\r\n\r\n"
                          "Access to this site is restricted.\r\n"
                          "Please provide credentials.\r\n", stdout);
                    fflush(stdout);
                    exit(0);
                }
            }
        }
    }
#endif                          /* AUTH */
    nurl = url + strlen(url);
    if (nurl > url)
        --nurl;
    if (*nurl == '/') {
        int             i;
        nurl = alloca(strlen(url) + 12);
        i = sprintf(nurl, "%sindex.html", url);
        url = nurl;
        nurl = url + i;
    }
    nurl -= 3;

    if (docgi) {
        char           *tmp,
                       *pathinfo;
        pathinfo = 0;
        for (tmp = url; tmp < nurl; ++tmp)
            if (findcgi(tmp)) {
                nurl = tmp;
                if (tmp[4] == '/')
                    pathinfo = tmp + 4;
                break;
            }
        if (pathinfo) {
            int             len = strlen(pathinfo) + 1;
            tmp = alloca(len);
            memcpy(tmp, pathinfo, len);
            *pathinfo = 0;
            pathinfo = tmp;
        }
        if (findcgi(nurl)) {
            int             i;
            if ((method == HEAD))
                badrequest(400, "Bad Request",
                           "Illegal HTTP method for Gateway call.");
#ifdef TCP_CORK
            {
                int             one = 1;
                setsockopt(1, IPPROTO_TCP, TCP_CORK, &one, sizeof(one));
            }
#endif
            for (i = nurl - url; i > -1; --i) {
                if ((nurl[0] == '/') && (nurl[1] == 'n')
                    && (nurl[2] == 'p') && (nurl[3] == 'h')
                    && (nurl[4] == '-'))
                    start_cgi(1, pathinfo, envp);       /* start a NPH-CGI 
                                                         */
                --nurl;
            }
          indexcgi:
            start_cgi(0, pathinfo, envp);       /* start a CGI */
        }
    }

    {
        int             fd;
        if ((fd = doit(buf, len, url, 1)) >= 0) {       /* file was there */
            /*
             * look if file.gz is also there and acceptable 
             */
            int             ul = strlen(url);
            char           *fnord = alloca(ul + 4);
            int             fd2;
            char           *oldencoding = encoding;

            strcpy(fnord, url);
            strcpy(fnord + ul, ".gz");
            fd2 = doit(buf, len, fnord, 0);
            if (fd2 >= 0) {     /* yeah! */
                url = fnord;
                close(fd);
                fd = fd2;
            } else {
                encoding = oldencoding;
            }
            retcode = 200;
            dolog(st.st_size);
            if (rangestart || rangeend != st.st_size)
                fputs("HTTP/1.0 206 Partial Content\r\n", stdout);
            else
                fputs("HTTP/1.0 200 OK\r\n", stdout);
            fputs("Server: " FNORD "\r\nContent-Type: ", stdout);
            fputs(mimetype, stdout);
            fputs("\r\n", stdout);
            switch (keepalive) {
            case -1:
                fputs("Connection: close\r\n", stdout);
                break;
            case 1:
                fputs("Connection: Keep-Alive\r\n", stdout);
                break;
            }
            if (encoding) {
                printf("Content-Encoding: %s\r\n", encoding);
            }
            printf("Content-Length: %llu\r\n",
                   (unsigned long long) (rangeend - rangestart));
            {
                /*
                 * glibc's gmtime parses tzinfo, resulting in 9
                 * additional syscalls.  uclibc doesn't do this.
                 * I presume dietlibc doesn't either.
                 */
                struct tm      *x = gmtime(&st.st_mtime);
                /*
                 * "Sun, 06 Nov 1994 08:49:37 GMT" 
                 */
                printf("Last-Modified: %.3s, %02d %.3s %d %02d:%02d:%02d GMT\r\n",
                       days + (3 * x->tm_wday),
                       x->tm_mday,
                       months + (3 * x->tm_mon),
                       x->tm_year + 1900,
                       x->tm_hour, x->tm_min, x->tm_sec);
            }
            if (rangestart || rangeend != st.st_size) {
                printf
                    ("Accept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\n",
                     (unsigned long long) rangestart,
                     (unsigned long long) rangeend - 1,
                     (unsigned long long) st.st_size);
            }
            fputs("\r\n", stdout);
            if (method == GET || method == POST) {
                switch (serve_static_data(fd)) {
                case 0:
                    break;
                case -1:
                    goto error500;
                case 1:
                    return 1;
                }
#ifdef TCP_CORK
                if (corked) {
                    int             zero = 0;
                    setsockopt(1, IPPROTO_TCP, TCP_CORK, &zero,
                               sizeof(zero));
                }
#endif
                if (keepalive > 0) {
                    close(fd);
                    fchdir(rootdir);
                    close(rootdir);
                    goto handlenext;
                }
                exit(0);
              error500:
                retcode = 500;
            } else
                fflush(stdout);
        }
    }
    switch (retcode) {
    case 404:
        {
            char           *space = alloca(strlen(url) + 2);

            if (handleindexcgi(url, origurl, space))
                goto indexcgi;
            handleredirect(url, origurl);
            if (dirlist) {
                handledirlist(origurl);
            }
            badrequest(404, "Not Found",
                       "<title>Not Found</title>No such file or directory.");
        }
    case 406:
        badrequest(406, "Not Acceptable",
                   "<title>Not Acceptable</title>Nothing acceptable found.");
    case 416:
        badrequest(416, "Requested Range Not Satisfiable", "");
    case 304:
        badrequest(304, "Not Changed", "");
    case 500:
        badrequest(500, "Internal Server Error", "");
    }
    return 1;
}
