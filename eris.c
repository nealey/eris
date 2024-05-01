/*
 * simple httpd to be started from tcpserver 
 */
#define _FILE_OFFSET_BITS 64
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
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

#include "strings.h"
#include "mime.h"
#include "timerfc.h"
#include "version.h"

#ifdef __linux__
#include <sys/sendfile.h>
#else
#define sendfile(a, b, c, d) -1
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

/*
 * Wait this long (seconds) for a valid HTTP request 
 */
#define READTIMEOUT 2

/*
 * Wait this long (seconds) for a non-file send to complete 
 */
#define WRITETIMEOUT 10

/*
 * Quit if we can't write at least this many bytes per second 
 */
#define MIN_WRITE_RATE 2560

/*
 * Wait this long for CGI to complete 
 */
#define CGI_TIMEOUT	(5*60)

/*
 * How long each sendfile call can take 
 */
#define SENDFILE_TIMEOUT ((int)(SIZE_MAX / MIN_WRITE_RATE))

/*
 * Maximum size of a request header (the whole block) 
 */
#define MAXHEADERLEN 8192

/*
 * Maximum size of a request line 
 */
#define MAXREQUESTLEN 2048

/*
 * Maximum number of header fields 
 */
#define MAXHEADERFIELDS 60

#define BUFFER_SIZE 8192

/*
 * Options
 */
int doauth = 0;
int docgi = 0;
int doidx = 0;
int nochdir = 0;
int redirect = 0;
int portappend = 0;
char *connector = NULL;


/*
 * Variables that persist between requests 
 */
int cwd;
int keepalive = 0;
char *remote_addr = NULL;
char *remote_ident = NULL;

/*
 * Things that are really super convenient to have globally.
 * These could be put into a struct for a threading version
 * of eris.
 */
enum { GET, POST, HEAD, CONNECT } method;
char *host;
char *user_agent;
char *refer;
char *path;
int http_version;
char *content_type;
size_t content_length;
off_t range_start, range_end;
time_t ims;


#define BUFFER_SIZE 8192


/** Log a request */
void
dolog(int code, off_t len)
{				/* write a log line to stderr */
	sanitize(host);
	sanitize(user_agent);
	sanitize(refer);

	fprintf(stderr, "%s %d %lu %s %s %s %s\n", remote_addr, code, (unsigned long) len, host, user_agent, refer, path);
}

void
header(unsigned int code, const char *httpcomment)
{
	printf("HTTP/1.%d %u %s\r\n", http_version, code, httpcomment);
	printf("Server: %s\r\n", FNORD);
	printf("Connection: %s\r\n", keepalive ? "keep-alive" : "close");

}

void
eoh()
{
	printf("\r\n");
}

/*
 * output an error message and exit 
 */
void
badrequest(long code, const char *httpcomment, const char *message)
{
	size_t msglen = 0;

	keepalive = 0;
	header(code, httpcomment);
	if (message) {
		msglen = (strlen(message) * 2) + 15;

		printf("Content-Length: %lu\r\nContent-Type: text/html\r\n\r\n", (unsigned long) msglen);
		printf("<title>%s</title>%s", message, message);
	}
	printf("\r\n");
	fflush(stdout);
	dolog(code, msglen);

	exit(0);
}

void
env(const char *k, const char *v)
{
	if (v) {
		setenv(k, v, 1);
	}
}


void
not_found()
{
	char msg[] = "The requested URL does not exist here.";

	header(404, "Not Found");
	printf("Content-Type: text/html\r\n");
	printf("Content-Length: %lu\r\n", (unsigned long) sizeof msg);
	printf("\r\n");
	printf("%s\n", msg);	/* sizeof msg includes the NULL */
	dolog(404, sizeof msg);
	fflush(stdout);
}

char *
proto_getenv(char *proto, char *name)
{
	char buf[80];

	snprintf(buf, sizeof buf, "%s%s", proto, name);
	return getenv(buf);
}

void
get_ucspi_env()
{
	char *ucspi = getenv("PROTO");
	char *ip = NULL;
	char *port = NULL;

	if (ucspi) {
		char *p;

		/*
		 * Busybox, as usual, has the right idea 
		 */
		if ((p = proto_getenv(ucspi, "REMOTEADDR"))) {
			remote_addr = strdup(p);
		} else {
			ip = proto_getenv(ucspi, "REMOTEIP");
			port = proto_getenv(ucspi, "REMOTEPORT");
		}

		if ((p = proto_getenv(ucspi, "REMOTEINFO"))) {
			remote_ident = strdup(p);
		}
	}

	if (!ip) {
		// stunnel
		ip = getenv("REMOTE_HOST");
		port = getenv("REMOTE_PORT");
	}

	if (ip) {
		char buf[80];

		snprintf(buf, sizeof buf, "%s:%s", ip, port);
		remote_addr = strdup(buf);
	}
}

void
parse_options(int argc, char *argv[])
{
	int opt;

	while (-1 != (opt = getopt(argc, argv, "acdhkpro:v."))) {
		switch (opt) {
		case 'a':
			doauth = 1;
			break;
		case 'c':
			docgi = 1;
			break;
		case 'd':
			doidx = 1;
			break;
		case '.':
			nochdir = 1;
			break;
		case 'p':
			portappend = 1;
			break;
		case 'r':
			redirect = 1;
			break;
		case 'o':
			connector = optarg;
			break;
		case 'v':
			printf("%s\n", FNORD);
			exit(0);
		case 'h':
		default:
			fprintf(stderr, "Usage: %s [OPTIONS]\n", argv[0]);
			fprintf(stderr, "\n");
			fprintf(stderr, "-a           Enable authentication\n");
			fprintf(stderr, "-c           Enable CGI\n");
			fprintf(stderr, "-d           Enable directory listing\n");
			fprintf(stderr, "-.           Serve out of ./ (no vhosting)\n");
			fprintf(stderr, "-h           Print this message and exit\n");
			fprintf(stderr, "-p           Append port to hostname directory\n");
			fprintf(stderr, "-r           Enable symlink redirection\n");
			fprintf(stderr, "-o HANDLER   Path to HTTP CONNECT handler\n");
			fprintf(stderr, "-v           Print version and exit\n");
			exit(69);
		}
	}
}

/*
 * CGI stuff
 */
static void
sigchld(int sig)
{
	while (waitpid(0, NULL, WNOHANG) > 0);
}

static void
sigalarm_cgi(int sig)
{
	/*
	 * send this out regardless of whether we've already sent a header, to maybe help with debugging 
	 */
	badrequest(504, "Gateway Timeout", "The CGI is being too slow.");
}

static void
cgi_child(const char *relpath)
{
	env("GATEWAY_INTERFACE", "CGI/1.1");
	env("SERVER_SOFTWARE", FNORD);
	env("REQUEST_URI", path);
	env("SERVER_NAME", host);
	env("SCRIPT_NAME", relpath);
	env("REMOTE_ADDR", remote_addr);
	env("REMOTE_IDENT", remote_ident);
	if (content_length) {
		char cl[20];

		snprintf(cl, sizeof cl, "%llu", (unsigned long long) content_length);
		env("CONTENT_LENGTH", cl);
		env("CONTENT_TYPE", content_type);
	}

	/*
	 * Try to change to CGI's directory 
	 */
	{
		char *delim = strrchr(relpath, '/');

		if (delim) {
			*delim = '\0';
			if (0 == chdir(relpath)) {
				relpath = delim + 1;
			}
		}
	}

	execl(relpath, relpath, NULL);
	exit(1);
}

void
cgi_parent(int cin, int cout, int passthru)
{
	char cgiheader[BUFFER_SIZE];
	size_t cgiheaderlen = 0;
	FILE *cinf = fdopen(cin, "rb");
	size_t size = 0;
	int header_sent = 0;
	int code = 200;

	fcntl(cin, F_SETFL, O_NONBLOCK);
	signal(SIGCHLD, sigchld);
	signal(SIGPIPE, SIG_IGN);	/* NO! no signal! */
	signal(SIGALRM, sigalarm_cgi);

	while (1) {
		int nfds;
		fd_set rfds, wfds;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(cin, &rfds);
		nfds = cin;

		if (content_length) {
			/*
			 * have post data 
			 */
			FD_SET(cout, &wfds);
			if (cout > nfds) {
				nfds = cout;
			}
		} else if (cout >= 0) {
			close(cout);	/* no post data */
			cout = -1;
		}

		alarm(CGI_TIMEOUT);
		if (-1 == select(nfds + 1, &rfds, &wfds, NULL, NULL)) {
			break;
		}

		if (FD_ISSET(cin, &rfds)) {
			if (passthru) {
				/*
				 * Pass everything through verbatim 
				 */
				size_t len;

				/*
				 * Re-use this big buffer 
				 */
				len = fread(cgiheader, 1, sizeof cgiheader, cinf);
				if (0 == len) {
					/*
					 * CGI is done 
					 */
					break;
				}
				fwrite(cgiheader, 1, len, stdout);

				/*
				 * Naively assume the CGI knows best about sending stuff 
				 */
				fflush(stdout);
				size += len;
			} else {
				/*
				 * Interpret header fields 
				 */
				size_t readlen = (sizeof cgiheader) - cgiheaderlen;

				if (NULL == fgets(cgiheader + cgiheaderlen, readlen, cinf)) {
					/*
					 * EOF or error 
					 */
					badrequest(500, "CGI Error", "CGI output too weird");
				}
				cgiheaderlen = strlen(cgiheader);

				if ('\n' == cgiheader[cgiheaderlen - 1]) {
					/*
					 * We read a whole line 
					 */
					size_t len;
					char *val;

					len = extract_header_field(cgiheader, &val, 0);
					if (!len) {
						/*
						 * We've read the entire header block 
						 */
						passthru = 1;
						eoh();
					} else {
						if (!header_sent) {
							if (!strcasecmp(cgiheader, "Location")) {
								header(302, "CGI Redirect");
								printf("%s: %s\r\n\r\n", cgiheader, val);
								dolog(302, 0);
								exit(0);
							} else if (!strcasecmp(cgiheader, "Status")) {
								char *txt;

								if (val) {
									code = (int) strtol(val, &txt, 10);
								} else {
									code = 0;
								}
								if (code < 100) {
									header(500, "Internal Error");
									printf("CGI returned Status: %d\n", code);
									dolog(500, 0);
									exit(0);
								}
								for (; *txt == ' '; txt += 1);
								header(code, txt);
							} else {
								header(200, "OK");
								printf("Pragma: no-cache\r\n");
							}
							header_sent = 1;
						}
						printf("%s: %s\r\n", cgiheader, val);
						cgiheaderlen = 0;
					}
				}
			}
		} else if (FD_ISSET(cout, &wfds)) {
			/*
			 * write to cgi the post data 
			 */
			if (content_length) {
				size_t len;
				char buf[BUFFER_SIZE];
				size_t nmemb = min(BUFFER_SIZE, content_length);
				char *p = buf;

				len = fread(buf, 1, nmemb, stdin);
				if (len < 1) {
					break;
				}
				content_length -= len;

				while (len > 0) {
					size_t wlen = write(cout, p, len);

					if (wlen == -1) {
						break;
					}
					len -= wlen;
					p += wlen;
				}
			} else {
				close(cout);
			}
		}
	}

	fflush(stdout);
	dolog(code, size);
}

void
serve_cgi(char *relpath)
{
	int pid;
	int cin[2];
	int cout[2];

	if (pipe(cin) || pipe(cout)) {
		badrequest(500, "Internal Server Error", "Server Resource problem.");
	}

	pid = fork();
	if (-1 == pid) {
		badrequest(500, "Internal Server Error", "Unable to fork.");
	}
	if (pid) {
		close(cin[1]);
		close(cout[0]);

		/*
		 * Eris is not this smart yet 
		 */
		keepalive = 0;

		cgi_parent(cin[0], cout[1], 0);

		exit(0);
	} else {
		close(cwd);
		close(cout[1]);
		close(cin[0]);

		dup2(cout[0], 0);
		dup2(cin[1], 1);

		close(cout[0]);
		close(cin[1]);

		cgi_child(relpath);
	}
}

/*
 * Main HTTPd
 */

ssize_t
fake_sendfile(int out_fd, int in_fd, off_t * offset, size_t count)
{
	char buf[BUFFER_SIZE];
	ssize_t l, m;

	/*
	 * is mmap quicker? does it matter? 
	 */
	if (-1 == lseek(in_fd, *offset, SEEK_SET)) {
		/*
		 * We're screwed.  The most helpful thing we can do now is die. 
		 */
		fprintf(stderr, "Unable to seek.  Dying.\n");
		exit(0);
	}
	l = read(in_fd, buf, min(count, sizeof buf));
	if (-1 == l) {
		/*
		 * Also screwed. 
		 */
		fprintf(stderr, "Unable to read an open file.  Dying.\n");
		exit(0);
	}
	*offset += l;

	while (l) {
		m = write(out_fd, buf, l);
		if (-1 == m) {
			/*
			 * ALSO screwed. 
			 */
			fprintf(stderr, "Unable to write to client: %m (req %s).  Dying.\n", path);
			exit(0);
		}
		l -= m;
	}

	return l;
}

void
serve_file(int fd, char *filename, struct stat *st)
{
	off_t len, remain;

	if (method == POST) {
		badrequest(405, "Method Not Supported", "POST is not supported by this URL");
	}

	if (st->st_mtime <= ims) {
		header(304, "Not Changed");
		dolog(304, 0);
		eoh();
		return;
	}

	header(200, "OK");
	printf("Content-Type: %s\r\n", getmimetype(filename));

	if ((range_end == 0) || (range_end > st->st_size)) {
		range_end = st->st_size;
	}
	len = range_end - range_start;
	printf("Content-Length: %llu\r\n", (unsigned long long) len);

	{
		struct tm *tp;
		char buf[40];

		tp = gmtime(&(st->st_mtime));

		strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", tp);
		printf("Last-Modified: %s\r\n", buf);
	}

	eoh();
	fflush(stdout);

	if (method == HEAD) {
		return;
	}

	for (remain = len; remain;) {
		size_t count = min(remain, SIZE_MAX);
		ssize_t sent;

		alarm(SENDFILE_TIMEOUT);
		sent = sendfile(1, fd, &range_start, count);
		if (-1 == sent) {
			sent = fake_sendfile(1, fd, &range_start, count);
		}
		remain -= sent;
	}

	dolog(200, len);
}

void
serve_idx(int fd, char *path)
{
	DIR *d = fdopendir(fd);
	struct dirent *de;

	if (method == POST) {
		badrequest(405, "Method Not Supported", "POST is not supported by this URL");
	}

	keepalive = 0;
	header(200, "OK");
	printf("Content-Type: text/html\r\n");
	eoh();

	printf("<!DOCTYPE html>\r<html><head><title>");
	html_esc(stdout, path);
	printf("</title></head><body><h1>Directory Listing: ");
	html_esc(stdout, path);
	printf("</h1><pre>\n");
	if (path[1]) {
		printf("<a href=\"../\">Parent Directory</a>\n");
	}

	while ((de = readdir(d))) {
		char *name = de->d_name;
		char symlink[PATH_MAX];
		struct stat st;

		if (name[0] == '.') {
			continue;	/* hidden files -> skip */
		}
		if (lstat(name, &st)) {
			continue;	/* can't stat -> skip */
		}

		if (S_ISDIR(st.st_mode)) {
			printf("[DIR]	 ");
		} else if (S_ISLNK(st.st_mode)) {
			ssize_t len = readlink(de->d_name, symlink, (sizeof symlink) - 1);

			if (len < 1) {
				continue;
			}
			name = symlink;
			printf("[LNK]	 ");	/* symlink */
		} else if (S_ISREG(st.st_mode)) {
			printf("%10llu", (unsigned long long) st.st_size);
		} else {
			continue;	/* not a file we can provide -> skip */
		}

		/*
		 * write a href 
		 */
		printf("  <a href=\"");
		url_esc(stdout, name);
		if (S_ISDIR(st.st_mode)) {
			printf("/");
		}
		printf("\">");
		url_esc(stdout, name);
		printf("</a>\n");
	}
	printf("</pre></body></html>");

	dolog(200, 0);
}

void
find_serve_file(char *relpath)
{
	int fd;
	struct stat st;

	/*
	 * Open fspath.  If that worked, 
	 */
	if ((fd = open(relpath, O_RDONLY)) > -1) {
		fstat(fd, &st);
		/*
		 * If it is a directory, 
		 */
		if (S_ISDIR(st.st_mode)) {
			char path2[PATH_MAX];
			int fd2;

			/*
			 * Redirect if it doesn't end with / 
			 */
			if (!endswith(path, "/")) {
				header(301, "Redirect");
				printf("Location: %s/\r\n", path);
				eoh();
				return;
			}

			/*
			 * Open relpath + "index.html".  If that worked,
			 */
			snprintf(path2, sizeof path2, "%sindex.html", relpath);
			if ((fd2 = open(path2, O_RDONLY)) > -1) {
				/*
				 * serve that file and return. 
				 */
				fstat(fd2, &st);
				serve_file(fd2, path2, &st);
				close(fd2);
				close(fd);
				return;
			} else {
				if (docgi) {
					snprintf(path2, sizeof path2, "%sindex.cgi", relpath);
					if (!stat(path2, &st)) {
						return serve_cgi(path2);
					}
				}
				if (doidx) {
					serve_idx(fd, relpath + 1);
					close(fd);
					return;
				}
				return not_found();
			}
		} else {
			if (docgi && endswith(relpath, ".cgi")) {
				close(fd);
				return serve_cgi(relpath);
			}
			serve_file(fd, relpath, &st);
		}
	} else {
		if (docgi && (errno == ENOTDIR)) {
			char *p;

			if ((p = strstr(relpath, ".cgi"))) {
				p += 4;
				env("PATH_INFO", p);
				*p = 0;
				if (!stat(relpath, &st)) {
					close(fd);
					return serve_cgi(relpath);
				}
			}
		}
		return not_found();
	}
}

void
handle_request()
{
	char request[MAXREQUESTLEN];
	char fspath[PATH_MAX];
	char buf[MAXHEADERLEN];
	char *p;

	/*
	 * Initialize globals 
	 */
	host = NULL;
	user_agent = NULL;
	refer = NULL;
	path = NULL;
	range_start = 0;
	range_end = 0;
	content_type = NULL;
	content_length = 0;
	ims = 0;

	alarm(READTIMEOUT);

	/*
	 * Read request line first 
	 */
	request[0] = 0;
	if (NULL == fgets(request, sizeof request, stdin)) {
		/*
		 * They must have hung up! 
		 */
		exit(0);
	}
	if (!strncmp(request, "GET /", 5)) {
		method = GET;
		p = request + 4;
	} else if (!strncmp(request, "POST /", 6)) {
		method = POST;
		p = request + 5;
	} else if (!strncmp(request, "HEAD /", 6)) {
		method = HEAD;
		p = request + 5;
	} else if (connector && !strncmp(request, "CONNECT ", 8)) {
		method = CONNECT;
		p = request + 8;
	} else {
		/*
		 * This also handles the case where fgets does nothing 
		 */
		badrequest(405, "Method Not Allowed", "Unsupported HTTP method.");
	}

	if (docgi) {
		p[-1] = 0;
		env("REQUEST_METHOD", request);
	}

	/*
	 * Interpret path into fspath. 
	 */
	path = p;
	{
		char *fsp = fspath;
		char *query_string = NULL;

		*(fsp++) = '.';
		for (; *p != ' '; p += 1) {
			char c = *p;

			switch (c) {
			case 0:
				badrequest(413, "Request Entity Too Large", "The HTTP request was too long");
			case '\n':
				badrequest(505, "Version Not Supported", "HTTP/0.9 not supported");
			case '?':
				query_string = p + 1;
				break;
			case '%':
				if ((!query_string) && p[1] && p[2]) {
					int a = fromhex(p[1]);
					int b = fromhex(p[2]);

					if ((a >= 0) && (b >= 0)) {
						c = (a << 4) | b;
						p += 2;
					}
				}
				break;
			}

			if ((!query_string) && (fsp - fspath + 1 < sizeof fspath)) {
				*(fsp++) = c;
			}
		}
		*fsp = 0;

		/*
		 * Change "/." to "/:" to keep "hidden" files such and prevent directory traversal 
		 */
		while ((fsp = strstr(fspath, "/."))) {
			*(fsp + 1) = ':';
		}


		*(p++) = 0;	/* NULL-terminate path */

		if (docgi && query_string) {
			env("QUERY_STRING", query_string);
		}
	}

	http_version = -1;
	if (!strncmp(p, "HTTP/1.", 7) && p[8] && ((p[8] == '\r') || (p[8] == '\n'))) {
		http_version = p[7] - '0';
	}
	if (!((http_version == 0) || (http_version == 1))) {
		http_version = 0;
		badrequest(505, "Version Not Supported", "HTTP version not supported");
	}
	if (http_version == 1) {
		keepalive = 1;
	} else {
		keepalive = 0;
	}
	if (docgi) {
		p[8] = 0;
		env("SERVER_PROTOCOL", p);
	}

	/*
	 * Read header fields 
	 */
	{
		char *base = buf;
		char *lastchar = base + (sizeof buf) - 2;
		int nheaders = 0;

		*lastchar = 0;
		while (1) {
			char *cgi_name = base;
			char *p;
			int plen = (sizeof buf) - (base - buf);
			char *name, *val;
			size_t len;

			/*
			 * 40 is totally arbitrary here. 
			 */
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
			if (*lastchar) {
				badrequest(431, "Request Header Too Large", "An HTTP header field was too large");
			}

			len = extract_header_field(p, &val, 1);
			if (!len) {
				/*
				 * blank line 
				 */
				break;
			}
			if (!val) {
				badrequest(400, "Invalid header", "Unable to parse header block");
			}

			name = p;

			/*
			 * Set up CGI environment variables 
			 */
			if (docgi) {
				env(cgi_name, val);
			}

			/*
			 * By default, re-use buffer space 
			 */
			base = cgi_name;

			/*
			 * Handle special header fields 
			 */
			if (!strcmp(name, "HOST")) {
				host = val;
				base = name + len + 1;
			} else if (!strcmp(name, "USER_AGENT")) {
				user_agent = val;
				base = name + len + 1;
			} else if (!strcmp(name, "REFERER")) {
				refer = val;
				base = name + len + 1;
			} else if (!strcmp(name, "CONTENT_TYPE")) {
				content_type = val;
				base = name + len + 1;
			} else if (!strcmp(name, "CONTENT_LENGTH")) {
				content_length = (size_t) strtoull(val, NULL, 10);
			} else if (!strcmp(name, "CONNECTION")) {
				if (!strcasecmp(val, "keep-alive")) {
					keepalive = 1;
				} else {
					keepalive = 0;
				}
			} else if (!strcmp(name, "IF_MODIFIED_SINCE")) {
				ims = timerfc(val);
			} else if (!strcmp(name, "RANGE")) {
				/*
				 * Range: bytes=17-23 
				 */
				/*
				 * Range: bytes=23- 
				 */
				if (!strncmp(val, "bytes=", 6)) {
					p = val + 6;
					range_start = (off_t) strtoull(p, &p, 10);
					if (*p == '-') {
						range_end = (off_t) strtoull(p + 1, NULL, 10);
					} else {
						range_end = 0;
					}
				}
			}
		}
	}

	/*
	 * Try to change into the appropriate directory 
	 */
	if (!nochdir) {
		char fn[PATH_MAX];

		if (host) {
			snprintf(fn, sizeof(fn), "%s", host);
		} else {
			fn[0] = 0;
		}
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
			case 'A' ... 'Z':
				*p ^= ' ';
				break;
			}
		}

		if ((-1 == chdir(fn)) && (-1 == chdir("default"))) {
			badrequest(404, "Not Found", "This host is not served here");
		}
	}

	if (method == CONNECT) {
		execl(connector, connector, path, NULL);
		badrequest(500, "Unable to exec connector", strerror(errno));
	}

	/*
	 * Serve the file 
	 */
	alarm(WRITETIMEOUT);
	find_serve_file(fspath);
	fflush(stdout);

	return;
}

int
main(int argc, char *argv[], const char *const *envp)
{
	parse_options(argc, argv);

	cwd = open(".", O_RDONLY);

	signal(SIGPIPE, SIG_IGN);
	get_ucspi_env();

	while (1) {
		handle_request();
		if (!keepalive) {
			break;
		}
		if (-1 == fchdir(cwd)) {
			break;
		}
	}

	return 0;
}
