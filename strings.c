int
endswith(char *haystack, char *needle)
{
    char *h, *n;

    for (h = haystack; *h; h++);
    for (n = needle;   *n; n++);

    if (h - haystack < n - needle) {
        return 0;
    }

    while (n >= needle) {
        if (*(n--) != *(h--)) {
            return 0;
        }
    }

    return 1;
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
            if (buf[len] == '\n') {
                /* Blank line or incorrectly-formatted header */
                break;
            } else if (buf[len] == ':') {
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

void
html_esc(FILE *f, char *s)
{
    for (; *s; s += 1) {
        switch (*s) {
            case '<':
                fprintf(f, "&lt;");
                break;
            case '>':
                fprintf(f, "&gt;");
                break;
            case '&':
                fprintf(f, "&amp;");
                break;
            default:
                fputc(*s, f);
                break;
        }
    }
}

void
url_esc(FILE *f, char *s)
{
    for (; *s; s += 1) {
        if ((*s == '%') || (*s == 127) || (*s < 31)) {
            fprintf(f, "%%%02x", *s);
        } else {
            fputc(*s, f);
        }
    }
}
