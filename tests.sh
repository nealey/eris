#! /bin/sh

: ${HTTPD:=./eris}
: ${HTTPD_CGI:=./eris -c}
: ${HTTPD_IDX:=./eris -d}

H () {
    echo
    echo "$@"
    echo "==================================="
    echo
}

BR () {
    echo
    echo "-----------------------------------"
    echo
}
    
title() {
    printf "* %-50s: " "$1"
    tests=$(expr $tests + 1)
}

successes=0
pass () {
    echo 'ok'
    successes=$(expr $successes + 1)
}

failures=0
fail () {
    echo 'FAIL'
    failures=$(expr $failures + 1)
}

d () {
    tr '\r\n' '#%'
}


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

echo "HTTPD: $HTTPD  "
echo "CGI:   $HTTPD_CGI  "
echo "IDX:   $HTTPD_IDX  "

H "Basic tests"

title "GET"
printf 'GET / HTTP/1.0\r\n\r\n' | $HTTPD 2>/dev/null | d | grep -q 'HTTP/1.0 200 OK#%Server: [a-z]*/[0-9.]*#%Content-Type: text/html; charset=UTF-8#%Content-Length: 6#%Last-Modified: ..., .. ... 20.. ..:..:.. GMT#%#%james%' && pass || fail

title "POST"
printf 'POST / HTTP/1.0\r\nContent-Type: a\r\nContent-Length: 5\r\n\r\njames' | $HTTPD 2>/dev/null | d | grep -q 'HTTP/1.0 200 OK#%Server: [a-z]*/[0-9.]*#%Content-Type: text/html; charset=UTF-8#%Content-Length: 6#%Last-Modified: ..., .. ... 20.. ..:..:.. GMT#%#%james%' && pass || fail

title "Logging /"
(printf 'GET / HTTP/1.1\r\nHost: host\r\n\r\n' | 
    PROTO=TCP TCPREMOTEPORT=1234 TCPREMOTEIP=10.0.0.2 $HTTPD >/dev/null) 2>&1 | grep -q '^10.0.0.2 200 6 host (null) (null) /$' && pass || fail

title "Logging /index.html"
(printf 'GET /index.html HTTP/1.1\r\nHost: host\r\n\r\n' | 
    PROTO=TCP TCPREMOTEPORT=1234 TCPREMOTEIP=10.0.0.2 $HTTPD >/dev/null) 2>&1 | grep -q '^10.0.0.2 200 6 host (null) (null) /index.html$' && pass || fail

H "fnord bugs"

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

BR
echo "$successes of $tests tests passed ($failures failed)."

exit $failures
