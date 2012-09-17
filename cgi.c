void
sigchld(int sig)
{
    while (waitpid(0, NULL, WNOHANG) > 0);
}

void
sigalarm_cgi(int sig)
{
    /* send this out regardless of whether we've already sent a header,
     * to maybe help with debugging */
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

    execl(relpath, relpath, NULL);
    exit(1);
}

void
cgi_parent(int cin, int cout, int passthru)
{
    char            cgiheader[BUFFER_SIZE];
    size_t          cgiheaderlen = 0;
    FILE           *cinf = fdopen(cin, "rb");
    size_t          size = 0;
    int             header_sent = 0;

    fcntl(cin, F_SETFL, O_NONBLOCK);
    signal(SIGCHLD, sigchld);
    signal(SIGPIPE, SIG_IGN);   /* NO! no signal! */

    while (1) {
        int             nfds;
        fd_set          rfds, wfds;
        
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(cin, &rfds);
        nfds = cin;
        
        if (content_length) {
            /* have post data */
            FD_SET(cout, &wfds);
            if (cout > nfds) {
                nfds = cout;
            }
        } else if (cout >= 0) {
            close(cout);   /* no post data */
            cout = -1;
        }

        if (-1 == select(nfds+1, &rfds, &wfds, NULL, NULL)) {
            break;
        }

        if (FD_ISSET(cin, &rfds)) {
            if (passthru) {
                /* Pass everything through verbatim */
                size_t len;

                /* Re-use this big buffer */
                len = fread(cgiheader, 1, sizeof cgiheader, cinf);
                if (0 == len) {
                    /* CGI is done */
                    break;
                }
                fwrite(cgiheader, 1, len, stdout);

                /* Naively assume the CGI knows best about sending stuff */
                fflush(stdout);
                size += len;
            } else {
                /* Interpret header fields */
                size_t readlen = (sizeof cgiheader) - cgiheaderlen;

                if (NULL == fgets(cgiheader + cgiheaderlen, readlen, cinf)) {
                    /* EOF or error */
                    badrequest(500, "CGI Error", "CGI output too weird");
                }
                cgiheaderlen = strlen(cgiheader);

                if ('\n' == cgiheader[cgiheaderlen - 1]) {
                    /* We read a whole line */
                    size_t len;
                    char *val;

                    len = extract_header_field(cgiheader, &val, 0);
                    if (! len) {
                        /* We've read the entire header block */
                        passthru = 1;
                        eoh();
                    } else {
                        if (! header_sent) {
                            if (! strcasecmp(cgiheader, "Location")) {
                                header(302, "CGI Redirect");
                                printf("%s: %s\r\n\r\n", cgiheader, val);
                                dolog(302, 0);
                                exit(0);
                            }
                        
                            header(200, "OK");
                            printf("Pragma: no-cache\r\n");

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

                len = fread(buf, 1, nmemb, stdin);
                if (len < 1) {
                    break;
                }
                content_length -= len;
                write(cout, buf, len);
            } else {
                close(cout);
            }
        }
    }

    fflush(stdout);
    dolog(200, size);
    cork(0);
}

void
serve_cgi(char *relpath)
{
    int             pid;
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

        /* Eris is not this smart yet */
        keepalive = 0;

        alarm(CGI_TIMEOUT);
        signal(SIGALRM, sigalarm_cgi);
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


