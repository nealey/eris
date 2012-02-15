VERSION := $(shell head -n 1 CHANGES | tr -d :)

CFLAGS = -DFNORD='"fnord/$(VERSION)"' -Wall -Werror

all: fnord

clean:
	rm -f *.[oa] fnord
