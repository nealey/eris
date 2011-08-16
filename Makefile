CC=gcc
CXX=g++

#LIBOWFAT=../libowfat/
#DIET=diet -Os

CFLAGS += -Os -fomit-frame-pointer
#CFLAGS=-g

all: fnord fnord-cgi fnord-idx

fnord: httpd
	cp -p $^ $@
	-strip -R .note -R .comment $@

httpd: httpd.o libowfat.a
	$(DIET) $(CC) -o $@ $^ $(CFLAGS)

fnord-cgi: httpd-cgi.o libowfat.a
	$(DIET) $(CC) -o $@ $^ $(CFLAGS)
	-strip -R .note -R .comment $@

fnord-idx: httpd-idx.o libowfat.a
	$(DIET) $(CC) -o $@ $^ $(CFLAGS)
	-strip -R .note -R .comment $@

libowfat.a: httpd.o buffer_1.o buffer_puts.o buffer_flush.o buffer_put.o \
buffer_putulong.o buffer_2.o buffer_putspace.o buffer_stubborn.o \
buffer_putflush.o str_copy.o fmt_ulong.o byte_diff.o byte_copy.o \
str_len.o str_diff.o str_chr.o str_diffn.o str_start.o scan_ulong.o
	ar cru $@ $^
	-ranlib $@

httpd.o: httpd.c
	$(DIET) $(CC) -pipe $(CFLAGS) -c $^ -DFNORD=\"fnord/$(shell head -n 1 CHANGES|sed 's/://')\"

httpd-cgi.o: httpd.c
	$(DIET) $(CC) -pipe $(CFLAGS) -c httpd.c -o $@ -DCGI -DFNORD=\"fnord/$(shell head -n 1 CHANGES|sed 's/://')\"

httpd-idx.o: httpd.c
	$(DIET) $(CC) -pipe $(CFLAGS) -c httpd.c -o $@ -DDIR_LIST -DFNORD=\"fnord/$(shell head -n 1 CHANGES|sed 's/://')\"

%.o: %.c
	$(DIET) $(CC) -pipe $(CFLAGS) -c $^

%.o: %.cpp
	$(DIET) $(CXX) -pipe $(CFLAGS) -c $^

.PHONY: rename clean install server
server: fnord
	tcpserver -v -RHl localhost 0 8000 ./fnord

clean:
	rm -f *.[oa] httpd fnord fnord-cgi fnord-idx

install:
	test -d /command || mkdir /command

CURNAME=$(notdir $(shell pwd))
VERSION=fnord-$(shell head -n 1 CHANGES|sed 's/://')

tar: rename
	cd .. && tar cvvf $(VERSION).tar.bz2 --use=bzip2 --exclude CVS --exclude bin-* --exclude .cvsignore --exclude default $(VERSION)

rename:
	if test $(CURNAME) != $(VERSION); then cd .. && mv $(CURNAME) $(VERSION); fi
