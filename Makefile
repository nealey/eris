VERSION := $(shell head -n 1 CHANGES | tr -d :)

CFLAGS = -DFNORD='"fnord/$(VERSION)"' -Wall -Werror

all: fnord

test: fnord
	cd tests && python3 ./test.py

clean:
	rm -f *.[oa] fnord
