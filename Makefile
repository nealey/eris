VERSION := $(shell head -n 1 CHANGES | tr -d :)

CFLAGS = -DFNORD='"eris/$(VERSION)"' -Wall -Werror

all: eris

eris: eris.c strings.c mime.c time.c cgi.c
	$(CC) $(CFLAGS) -o $@ $<

test: eris
	sh ./test.sh

clean:
	rm -f *.[oa] eris
