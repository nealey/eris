#ifndef __STRINGS_H__
#define __STRINGS_H__

#include <stdio.h>

int endswith(char *haystack, char *needle);
void sanitize(char *s);
size_t extract_header_field(char *buf, char **val, int cgi);
int fromhex(int c);
void html_esc(FILE *f, char *s);
void url_esc(FILE *f, char *s);

#endif
