VERSION := $(shell head -n 1 CHANGES | tr -d :)

CFLAGS = -DFNORD='"eris/$(VERSION)"' -Wall -Werror

all: eris

test: eris
	sh ./test.sh

clean:
	rm -f *.[oa] eris
