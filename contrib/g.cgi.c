/** g.cgi - CGI interface to cgit and git-http-backend
  *
  * This is a simple CGI to invoke cgit with a configuration
  * file of your choice.  It will also invoke git-http-backend
  * if appropriate, which in my (very light) testing runs about
  * twice as fast as plain HTTP with git-update-server-info.
  */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Set these to appropriate paths */
#define CGIT_CONFIG "/home/neale/public_html/cgitrc"
#define GIT_PROJECT_ROOT "/home/neale/projects"

int
main(int argc, char *argv[])
{
    char *uri = getenv("REQUEST_URI");

    if (uri && strstr(uri, "git-upload-pack")) {
        /* Use git-http-backend for great speed! */
        setenv("GIT_PROJECT_ROOT", GIT_PROJECT_ROOT, 1);
        execlp("git", "git", "http-backend", NULL);
    } else {
        setenv("CGIT_CONFIG", CGIT_CONFIG, 1);
        execlp("cgit", "cgit", NULL);
    }

    return 0;
}
