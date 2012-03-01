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

char           *
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

#if 0

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
    cgi_env[i++] = env_append("SCRIPT_NAME", path);
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

        DUMP_s(rpath);
        i = strrchr(rpath, '/') - rpath;
        if (i) {
            strncpy(tmp, rpath + 1, i);
            tmp[i] = 0;
            chdir(tmp);
        }
    }

    {
        char            tmp[PATH_MAX];

        /*
         * program name 
         */
        cgi_arg[0] = tmp;
        tmp[0] = '.';
        strcpy(tmp + 1, rpath + i);

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
            if (WTERMSIG(n) == SIGALRM) {
                /* XXX: should kill children here */
                badrequest(504, "Gateway Timeout",
                           "Gateway has run too long.");
            } else {
                badrequest(502, "Bad Gateway",
                           "Gateway broken or unavailable.");
            }
        }
    }
    signal(SIGCHLD, cgi_child);
}

static void
start_cgi(int nph, const char *pathinfo, const char *const *envp)
{
    // XXX: would it be safe to reuse headerbuf from main?
    size_t          size = 0;
    int             pid;
    char            cgiheader[BUFFER_SIZE];
    size_t          cgiheaderlen = BUFFER_SIZE;
    int             cin[2];
    int             cout[2];
    FILE           *cinf;

    if (pipe(cin) || pipe(cout) || !(cinf = fdopen(cin[0], "rb"))) {
        badrequest(500, "Internal Server Error",
                   "Server Resource problem.");
    }

    pid = fork();
    if (-1 == pid) {
        badrequest(500, "Internal Server Error",
                   "Unable to fork.");
    }
    if (pid) {
        /* Parent */
        int             passthru = nph;

        fcntl(cin[0], F_SETFL, O_NONBLOCK);
        signal(SIGCHLD, cgi_child);
        signal(SIGPIPE, SIG_IGN);   /* NO! no signal! */

        close(cin[1]);
        close(cout[0]);

        alarm(CGI_TIMEOUT);

        while (1) {
            int             nfds;
            fd_set          rfds, wfds;
            
            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            FD_SET(cin[0], &rfds);
            nfds = cin[0];
            
            if (post_len) {
                /* have post data */
                FD_SET(cout[1], &wfds);
                if (cout[1] > nfds) {
                    nfds = cout[1];
                }
            } else if (cout[1] >= 0) {
                close(cout[1]);   /* no post data */
                cout[1] = -1;
            }

            if (-1 == select(nfds+1, &rfds, &wfds, NULL, NULL)) {
                break;
            }

            if (FD_ISSET(cin[0], &rfds)) {
                if (passthru) {
                    size_t len;

                    /* Re-use this big buffer */
                    len = fread(cgiheader, 1, sizeof cgiheader, cinf);
                    if (0 == len) {
                        /* CGI is done */
                        break;
                    }
                    fwrite(cgiheader, 1, len, stdout);
                    size += len;
                } else {
                    int ret;    

                    ret = read_header(cinf, cgiheader, &cgiheaderlen);
                    if (0 == ret) {
                        /* Call read_header again */
                    } else if (-1 == ret) {
                        /* EOF or error */
                        badrequest(500, "CGI Error",
                                   "CGI output too weird");
                    } else {
                        /* Entire header is in memory now */
                        passthru = 1;
                        
                        /* XXX: I think we need to look for Location:
                         * anywhere, but fnord got away with checking
                         * only the first header field, so I will too.
                         */
                        if (memcmp(cgiheader, "Location: ", 10) == 0) {
                            retcode = 302;
                            printf
                                ("HTTP/1.0 302 CGI-Redirect\r\nConnection: close\r\n");
                            fwrite(cgiheader, 1, cgiheaderlen, stdout);
                            dolog(0);
                            exit(0);
                        }

                        retcode = 200;
                        printf("HTTP/1.0 200 OK\r\nServer: "
                               FNORD
                               "\r\nPragma: no-cache\r\nConnection: close\r\n");
                        signal(SIGCHLD, SIG_IGN);
                        fwrite(cgiheader, 1, cgiheaderlen, stdout);
                    }
                }
            } else if (FD_ISSET(cout[1], &wfds)) {
                /*
                 * write to cgi the post data 
                 */
                if (post_len) {
                    size_t len;
                    char buf[BUFFER_SIZE];
                    size_t nmemb = min(BUFFER_SIZE, post_len);

                    len = fread(buf, 1, nmemb, stdin);
                    if (len < 1) {
                        break;
                    }
                    post_len -= len;
                    write(cout[1], buf, len);
                } else {
                    close(cout[1]);
                }
            }
        }
        alarm(0);

        fflush(stdout);
        dolog(size);
        cork(0);
    } else {
        /* Child */

        close(cout[1]);
        close(cin[0]);

        dup2(cout[0], 0);
        dup2(cin[1], 1);

        close(cout[0]);
        close(cin[1]);

        do_cgi(pathinfo, envp);
    }
    exit(0);
}
#endif

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

#if 0
/*
 * try to return a file 
 */
static int
doit(char *headerbuf, size_t headerlen, char *url)
{
    int             fd = -1;
    char           *accept;

    while (url[0] == '/')
        ++url;
    if ((fd = open(url, O_RDONLY)) >= 0) {
        if (fstat(fd, &st))
            goto bad;
        /*
         * no directories 
         */
        if (S_ISDIR(st.st_mode))
            goto bad;
        /*
         * see if the document has been changed 
         */
        {
            char           *field =
                header(headerbuf, headerlen, "If-Modified-Since");

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
        if ((accept = header(headerbuf, headerlen, "Range"))) {
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

    strncpy(rpath, test, sizeof rpath);
    return 1;                   /* Wow... now start "index.cgi" */
}

static int
findcgi(const char *c)
{
    return (c[0] == '.' && c[1] == 'c' &&
            c[2] == 'g' && c[3] == 'i' && (c[4] == '/' || c[4] == 0));
}


void
fake_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
    char buf[BUFFER_SIZE];
    ssize_t l, m;

    /* is mmap quicker?  does it matter? */
    if (rangestart && (*offset == rangestart)) {
        if (-1 == lseek(in_fd, *offset, SEEK_SET)) {
            /* We're screwed.  The most helpful thing we can do now is die. */
            fprintf(stderr, "Unable to seek.  Dying.\n");
            exit(0);
        }
    }
    l = read(in_fd, buf, min(count, sizeof buf));
    if (-1 == l) {
        /* Also screwed. */
        fprintf(stderr, "Unable to read an open file.  Dying.\n");
        exit(0);
    }
    *offset += l;

    while (l) {
        m = write(out_fd, buf, l);
        if (-1 == m) {
            /* ALSO screwed. */
            fprintf(stderr, "Unable to write to client.  Dying.\n");
            exit(0);
        }
        l -= m;
    }
}

/*
 * write from offset "rangestart" to offset "rangeend" to stdout
 */
static int
serve_static_data(int fd)
{
    off_t           len = rangeend - rangestart;
    off_t           offset = rangestart;

    /* XXX: A comment here asserted that files with length < 4096 were
     * faster to send with a read and write.  Test this. */

    cork(1);
    fflush(stdout);

    while (len) {
        size_t count = min(len, SIZE_MAX);
        ssize_t sent;

        sent = sendfile(1, fd, &offset, count);
        if (sent == -1) {
            fake_sendfile(1, fd, &offset, count);
        }
        len -= count;
    }
    cork(0);

    return 0;
}

#endif

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
    char request[MAXREQUESTLEN];
    char fspath[MAXREQUESTLEN];
    char buf[MAXHEADERLEN];
    char *p;
    char *query_string = NULL;
    int method;

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
    DUMP_d(http_version);

    /* Read header fields */
    {
        size_t offset = 0;

        while (1) {
            char   *cgi_name = buf + offset;
            char   *p;
            int     plen = (sizeof buf) - offset;
            char   *name, *val;
            size_t  len;

            if (plen < 40) {
                badrequest(431, "Request Header Too Large", "The HTTP header block was too large");
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
            DUMP_u(len);
            DUMP_s(cgi_name);
            DUMP_s(name);
            DUMP_s(val);
            if (! val) {
                badrequest(400, "Invalid header", "Unable to parse header block");
            }

            if (docgi) {
                /* Set this up for a later call to exec */
                setenv(cgi_name, val, 1);
            }

        }
    }

    return;
}

int
main(int argc, char *argv[], const char *const *envp)
{
    parse_options(argc, argv);

    setbuffer(stdout, stdout_buf, sizeof stdout_buf);

    signal(SIGPIPE, SIG_IGN);
    get_ucspi_env();

    do {
        handle_request();
    } while (keepalive);

    return 0;
}
