VERSION := $(shell head -n 1 CHANGES | tr -d :)

CFLAGS = -DFNORD='fnord/$(VERSION)'

all: fnord fnord-cgi fnord-idx

fnord-cgi: httpd.c
fnord-cgi: CFLAGS += -DCGI

fnord-idx: httpd.c
fnord-idx: CFLAGS += -DDIR_LIST

clean:
	rm -f *.[oa] fnord fnord-cgi fnord-idx
