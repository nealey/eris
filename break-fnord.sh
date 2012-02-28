#! /bin/sh

## Breaking fnord 1.10
##
## Run this as "HTTPD=../eris ./break-fnord.sh" if you'd like to
## run these tests against a built eris HTTPD.  It will fail the
## Accept test since eris ignores this.

if [ "$1" = "clean" ]; then
    rm -rf fnord-1.10
fi

# Set HTTPD= to test something else
case ${HTTPD:=./fnord} in
    *fnord)
        : ${HTTPD_IDX:=$HTTPD-idx}
        : ${HTTPD_CGI:=$HTTPD-cgi}
        ;;
    *eris)
        : ${HTTPD_IDX:=$HTTPD -d}
        : ${HTTPD_CGI:=$HTTPD -c}
        ;;
esac

title() {
    printf "%-50s: " "$1"
    tests=$(expr $tests + 1)
}

successes=0
pass () {
    echo 'pass'
    successes=$(expr $successes + 1)
}

failures=0
fail () {
    echo 'fail'
    failures=$(expr $failures + 1)
}

d () {
    tr '\r\n' '#%'
}


if [ ! -f fnord-1.10.tar.bz2 ]; then
    wget http://www.fefe.de/fnord/fnord-1.10.tar.bz2
fi

if [ ! -f fnord-1.10/httpd.c ]; then
    rm -rf fnord-1.10
    bzcat fnord-1.10.tar.bz2 | tar xf -
fi

cd fnord-1.10
make DIET=

if [ ! -d default ]; then
    mkdir default
    echo james > default/index.html
    touch default/a
    cat <<EOD > default/a.cgi
#! /bin/sh
echo 'Content-type: text/plain'
ls / > /dev/null   # delay a little
echo
echo james
EOD
    chmod +x default/a.cgi
    mkdir empty:80
fi

cat <<EOD


HTTPD: $HTTPD
CGI:   $HTTPD_CGI
IDX:   $HTTPD_IDX
-----------------------------------------
EOD

# 1. Should return directory listing of /; instead segfaults
title "Directory indexing of /"
printf 'GET / HTTP/1.0\r\nHost: empty\r\n\r\n' | $HTTPD_IDX 2>/dev/null | grep -q 200 && pass || fail

# 2. Should output \r\n\r\n; instead outputs \r\n\n
title "CGI output bare newlines"
printf 'GET /a.cgi HTTP/1.0\r\n\r\n' | $HTTPD_CGI 2>/dev/null | d | grep -q '#%#%' && pass || fail

# 3. Should process both requests; instead drops second
title "Multiple requests in one packet"
printf 'GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n' | $HTTPD 2>/dev/null | grep -c '^HTTP/1.' | grep -q 2 && pass || fail

# 4. Should return 406 Not Acceptable; instead ignores Accept header
title "Accept header"
printf 'GET / HTTP/1.0\r\nAccept: nothing\r\n\r\n' | $HTTPD 2>/dev/null | grep 406 && pass || fail

# 5. Should serve second request as default MIME-Type (text/plain); instead uses previous mime type
title "Second MIME-Type"
(printf 'GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n'
 ls / > /dev/null    # Delay required to work around test #3
 printf 'GET /a HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n') | $HTTPD 2>/dev/null | grep -q 'text/plain\|application/octet-stream' && pass || fail

# 6. Should consume POST data; instead tries to read POST data as second request
title "POST to static HTML"
(printf 'POST / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\nContent-Type: text/plain\r\nContent-Length: 1\r\n\r\n';
 ls / > /dev/null
 printf 'aPOST / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\nContent-Type: text/plain\r\nContent-Length: 1\r\n\r\na') | $HTTPD 2>/dev/null | grep -c '200 OK' | grep -q 2 && pass || fail

# 7. HTTP/1.1 should default to keepalive; instead connection is closed
title "HTTP/1.1 default keepalive"
(printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\n'
 ls / >/dev/null
 printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\n') | $HTTPD 2>/dev/null | grep -c '^HTTP/' | grep -q 2 && pass || fail

# 8. Should parse "Thursday"; instead assumes all day names are 6 characters long
title "RFC 850 Date"
printf 'GET / HTTP/1.0\r\nIf-Modified-Since: Thursday, 27-Feb-30 12:12:12 GMT\r\n\r\n' | $HTTPD 2>/dev/null | grep -q '304 Not Changed' && pass || fail

cat <<EOD
-----------------------------------------
$successes of $tests tests passed ($failures failed).
EOD

exit $failures
