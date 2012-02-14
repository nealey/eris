unsigned int
str_copy(char *out, const char *in)
{
    char *s = out;
    const char *t = in;

    while (1) {
        if (!(*s=*t)) break;
        s += 1;
        t += 1;
    }
    return s - out;
}

int
str_start(const char *s, const char *t)
{
    char x;

    while (1) {
        x = *t++;
        if (!x) return 1;
        if (x != *s++) return 0;
    }
}

