VERSION := $(shell head -n 1 CHANGES | tr -d :)

CFLAGS = -DFNORD='"eris/$(VERSION)"' -Wall -Werror

all: eris

eris: eris.o strings.o mime.o timerfc.o

test: eris
	sh ./test.sh

clean:
	rm -f *.[oa] eris
