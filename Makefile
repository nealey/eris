VERSION := $(shell head -n 1 CHANGES | tr -d :)

CFLAGS = -DFNORD='"eris/$(VERSION)"' -Wall -Werror

all: eris

test: eris
	cd tests && python3 ./test.py

clean:
	rm -f *.[oa] eris
