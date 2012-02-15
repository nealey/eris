VERSION := $(shell head -n 1 CHANGES | tr -d :)

CFLAGS = -DFNORD='"fnord/$(VERSION)"' -Wall -Werror

all: fnord fnord-cgi fnord-idx

fnord-cgi: CFLAGS += -DCGI
fnord-cgi: fnord.c
	$(CC) $(CFLAGS) -o $@ $(LDFLAGS) $<

fnord-idx: CFLAGS += -DDIR_LIST
fnord-idx: fnord.c
	$(CC) $(CFLAGS) -o $@ $(LDFLAGS) $<

clean:
	rm -f *.[oa] fnord fnord-cgi fnord-idx
