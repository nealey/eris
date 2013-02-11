CFLAGS = -Wall -Werror

all: eris

eris: eris.o strings.o mime.o timerfc.o

eris.o: version.h
version.h: CHANGES
	awk -F : 'NR==1 {printf("const char *FNORD = \"eris/%s\";\n", $$1);}' $< > $@

test: eris
	sh ./test.sh

clean:
	rm -f *.[oa] version.h eris
