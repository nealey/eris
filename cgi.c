void
sigchld(int sig)
{
    while (waitpid(0, NULL, WNOHANG) > 0);
}

static void
cgi_child(const char *relpath)
{
    setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
    setenv("SERVER_SOFTWARE", FNORD, 1);
    setenv("REQUEST_URI", path, 1);
    setenv("SERVER_NAME", host, 1);
    setenv("SCRIPT_NAME", relpath, 1);
    setenv("REMOTE_ADDR", remote_ip, 1);
    setenv("REMOTE_PORT", remote_port, 1);
    setenv("REMOTE_IDENT", remote_ident, 1);
    setenv("CONTENT_TYPE", content_type, 1);
    {
        char cl[20];

        snprintf(cl, sizeof cl, "%llu", (unsigned long long) content_length);
        setenv("CONTENT_LENGTH", cl, 1);
    }

    execl(relpath, relpath, NULL);
    exit(1);
}

void
cgi_parent(int cin, int cout)
{
    FILE           *cinf = fdopen(cin, "rb");
    int             passthru = nph;

    fcntl(child_in, F_SETFL, O_NONBLOCK);
    signal(SIGCHLD, sigchld);
    signal(SIGPIPE, SIG_IGN);   /* NO! no signal! */

    /* Eris is not this smart yet */
    keepalive = 0;

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
}
void
serve_cgi(char *relpath)
{
    size_t          size = 0;
    int             pid;
    char            buf[BUFFER_SIZE];
    int             cin[2];
    int             cout[2];

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

        alarm(CHILD_TIMEOUT);
        cgi_parent(cin[0], cout[1]);
        alarm(0);
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


