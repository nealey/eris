/*
 *   Code in this file is from mathopd <http://www.mathopd.org/>
 *
 *   Copyright 1996, Michiel Boland.
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

static const char days[] = "SunMonTueWedThuFriSat";
static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

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

char           *
rfctime(time_t t, char *buf)
{
    struct tm      *tp;

    /* ntp: in glibc, this triggers a bunch of needless I/O. */
    tp = gmtime(&t);
    if (tp == 0) {
        strcpy(buf, "?");
        return buf;
    }
    strftime(buf, 31, "%a, %d %b %Y %H:%M:%S GMT", tp);
    return buf;
}
