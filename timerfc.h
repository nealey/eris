#ifndef __TIMERFC_H__
#define __TIMERFC_H__

#include <time.h>

time_t timerfc(const char *s);
char *rfctime(time_t t, char *buf);

#endif
