#! /bin/sh

: ${HTTPD:=./eris}
: ${HTTPD_CGI:=./eris -c}
: ${HTTPD_IDX:=./eris -d}

H () {
    section="$*"
    printf "\n%-20s " "$*"
}

title() {
    thistest="$1"
    tests=$(expr $tests + 1)
}

successes=0
pass () {
    printf '.'
    successes=$(expr $successes + 1)
}

failures=0
fail () {
    printf '(%s)' "$thistest"
    failures=$(expr $failures + 1)
}

d () {
    tr '\r\n' '#%'
}


###
### Make web space
###
mkdir -p default
echo james > default/index.html
touch default/a

cat <<'EOD' > default/a.cgi
#! /bin/sh
echo 'Content-type: text/plain'
echo
set | sort
ls *.cgi
EOD
chmod +x default/a.cgi

cat <<'EOD' > default/mongo.cgi
#! /bin/sh
echo 'Content-type: application/octet-stream'
echo
dd if=/dev/zero bs=1000 count=800 2>/dev/null
echo 'james'
EOD
chmod +x default/mongo.cgi

cat <<'EOD' > default/redir.cgi
#! /bin/sh
echo "Location: http://example.com/froot"
EOD
chmod +x default/redir.cgi

mkdir -p default/empty
mkdir -p default/subdir
touch default/subdir/a
touch default/subdir/.hidden
###
###
###

echo "HTTPD: $HTTPD  "
echo "CGI:   $HTTPD_CGI  "
echo "IDX:   $HTTPD_IDX  "




H "Basic tests"

title "GET /index.html"
printf 'GET /index.html HTTP/1.0\r\n\r\n' | $HTTPD 2>/dev/null | d | grep -q 'HTTP/1.0 200 OK#%Server: eris/[0-9.a-z]*#%Connection: close#%Content-Type: text/html; charset=UTF-8#%Content-Length: 6#%Last-Modified: ..., .. ... 20.. ..:..:.. GMT#%#%james%' && pass || fail

title "GET /"
printf 'GET / HTTP/1.0\r\n\r\n' | $HTTPD 2>/dev/null | d | grep -q 'HTTP/1.0 200 OK#%Server: eris/[0-9.a-z]*#%Connection: close#%Content-Type: text/html; charset=UTF-8#%Content-Length: 6#%Last-Modified: ..., .. ... 20.. ..:..:.. GMT#%#%james%' && pass || fail

title "Keepalive"
printf 'GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n' | $HTTPD 2>/dev/null | grep -c 'james' | grep -q 2 && pass || fail

title "POST"
printf 'POST / HTTP/1.0\r\nContent-Type: a\r\nContent-Length: 5\r\n\r\njames' | $HTTPD 2>/dev/null | d | grep -q 'HTTP/1.0 405 ' && pass || fail

title "HTTP/1.2"
printf 'GET / HTTP/1.2\r\n\r\n' | $HTTPD 2>/dev/null | d | grep -q 'HTTP/1.. 505 .*ction: close' && pass || fail

title "HTTP/1.12"
printf 'GET / HTTP/1.12\r\n\r\n' | $HTTPD 2>/dev/null | d | grep -q 'HTTP/1.. 505 .*ction: close' && pass || fail

title "Bare newline"
printf 'GET / HTTP/1.0\n\n' | $HTTPD 2>/dev/null | grep -q 'james' && pass || fail

title "No trailing slash"
printf 'GET /empty HTTP/1.0\r\n\r\n' | $HTTPD 2>/dev/null | d | grep -q '301 Redirect#%.*Location: /empty/#%#%' && pass || fail

title "Logging /"
(printf 'GET / HTTP/1.1\r\nHost: host\r\n\r\n' | 
    PROTO=TCP TCPREMOTEPORT=1234 TCPREMOTEIP=10.0.0.2 $HTTPD >/dev/null) 2>&1 | grep -q '^10.0.0.2:1234 200 6 host (null) (null) /$' && pass || fail

title "Logging /index.html"
(printf 'GET /index.html HTTP/1.1\r\nHost: host\r\n\r\n' | 
    PROTO=TCP TCPREMOTEPORT=1234 TCPREMOTEIP=10.0.0.2 $HTTPD >/dev/null) 2>&1 | grep -q '^10.0.0.2:1234 200 6 host (null) (null) /index.html$' && pass || fail

title "Logging busybox"
(printf 'GET /index.html HTTP/1.1\r\nHost: host\r\n\r\n' | 
    PROTO=TCP TCPREMOTEADDR=[::1]:8765 $HTTPD >/dev/null) 2>&1 | grep -Fxq '[::1]:8765 200 6 host (null) (null) /index.html' && pass || fail



H "Options"

title "-."
printf 'GET /eris HTTP/1.0\r\n\r\n' | $HTTPD -. 2>/dev/null | grep -q 'HTTP/1.. 200 OK' && pass || fail



H "Tomfoolery"

title "Non-header"
printf 'GET / HTTP/1.0\r\na: b\r\nfoo\r\n\r\n' | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 400 ' && pass || fail

title "Huge header field"
(printf 'GET / HTTP/1.0\r\nHeader: '
 dd if=/dev/zero bs=1k count=9 2>/dev/null | tr '\0' '.'
 printf '\r\n\r\n') | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 431 ' && pass || fail

title "Too many headers"
(printf 'GET / HTTP/1.0\r\n'
 for i in $(seq 500); do
     printf 'Header: val\r\n'
 done
 printf '\r\n') | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 431 ' && pass || fail


H "If-Modified-Since"

title "Has been modified"
printf 'GET / HTTP/1.0\r\nIf-Modified-Since: Sun, 27 Feb 1980 12:12:12 GMT\r\n\r\n' | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 200 ' && pass || fail

title "Exact same date"
ims=$(printf 'GET / HTTP/1.0\r\n\r\n' | $HTTPD 2>/dev/null | awk -F ': ' '/Last-Modified/ {print $2;}')
printf 'GET / HTTP/1.0\r\nIf-Modified-Since: %s\r\n\r\n' "$ims" | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 304 ' && pass || fail

title "RFC 822 Date"
printf 'GET / HTTP/1.0\r\nIf-Modified-Since: Sun, 27 Feb 2030 12:12:12 GMT\r\n\r\n' | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 304 ' && pass || fail

title "RFC 850 Date"
printf 'GET / HTTP/1.0\r\nIf-Modified-Since: Sunday, 27-Feb-30 12:12:12 GMT\r\n\r\n' | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 304 ' && pass || fail

title "RFC 850 Thursday"
printf 'GET / HTTP/1.0\r\nIf-Modified-Since: Thursday, 27-Feb-30 12:12:12 GMT\r\n\r\n' | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 304 ' && pass || fail

title "ANSI C Date"
printf 'GET / HTTP/1.0\r\nIf-Modified-Since: Sun Feb 27 12:12:12 2030\r\n\r\n' | $HTTPD 2>/dev/null | grep -q 'HTTP/1.. 304 ' && pass || fail

title "ims persist"
printf 'GET / HTTP/1.1\r\nIf-Modified-Since: %s\r\n\r\nGET / HTTP/1.0\r\n\r\n' "$ims" | $HTTPD 2>/dev/null | d | grep -q 'HTTP/1.. 304.*HTTP/1.. 200' && pass || fail



H "Directory indexing"

title "Basic index"
printf 'GET /empty/ HTTP/1.0\r\n\r\n' | $HTTPD_IDX 2>/dev/null | d | grep -Fq '<h1>Directory Listing: /empty/</h1><pre><a href="../">Parent Directory</a>%</pre>' && pass || fail

title "Hidden file"
printf 'GET /subdir/ HTTP/1.0\r\n\r\n' | $HTTPD_IDX 2>/dev/null | grep -q 'hidden' && fail || pass



H "CGI"

title "Basic CGI"
printf 'GET /a.cgi HTTP/1.0\r\n\r\n' | \
    $HTTPD_CGI 2>/dev/null | d | grep -Eq 'HTTP/1.0 200 OK#%Server: .*#%Connection: close#%Pragma: no-cache#%Content-type: text/plain#%#%.*%GATEWAY_INTERFACE=.?CGI/1.1.?%' && pass || fail

title "CGI chdir"
printf 'GET /a.cgi HTTP/1.0\r\n\r\n' | \
    $HTTPD_CGI 2>/dev/null | d | grep -Eq '%a.cgi%' && pass || fail

title "REQUEST_METHOD"
printf 'GET /a.cgi HTTP/1.0\r\n\r\n' | \
    $HTTPD_CGI 2>/dev/null | grep -Eq 'REQUEST_METHOD=.?GET.?$' && pass || fail

title "GET with arguments"
printf 'GET /a.cgi?foo HTTP/1.0\r\n\r\n' | \
    $HTTPD_CGI 2>/dev/null | grep -Eq 'QUERY_STRING=.?foo.?$' && pass || fail

title "GET with complex args"
printf 'GET /a.cgi?t=New+Mexico+Land+Of+Enchantment&s=LG8+LV32+R4+G32+LG32+Y4+LG4 HTTP/1.0\r\n\r\n' | \
    $HTTPD_CGI 2>/dev/null | d | grep -Fq 't=New+Mexico' && pass || fail

title "POST"
printf 'POST /a.cgi HTTP/1.0\r\nContent-Type: moo\r\nContent-Length: 3\r\n\r\narf' | \
    $HTTPD_CGI 2>/dev/null | d | grep -Eq '%CONTENT_LENGTH=.?3.?%CONTENT_TYPE=.?moo.?%' && pass || fail

title "PATH_INFO"
printf 'GET /a.cgi/merf HTTP/1.0\r\n\r\n' | $HTTPD_CGI 2>/dev/null | grep -Eq '^PATH_INFO=.?/merf.?$' && pass || fail

title "SERVER_PROTOCOL"
printf 'GET /a.cgi HTTP/1.0\r\n\r\n' | $HTTPD_CGI 2>/dev/null | d | grep -Eq '%SERVER_PROTOCOL=.?HTTP/1.0[^#%]?%[^#%]' && pass || fail

title "Large response"
printf 'GET /mongo.cgi HTTP/1.0\r\n\r\n' | $HTTPD_CGI 2>/dev/null | grep -q james && pass || fail

title "Redirect"
printf 'GET /redir.cgi HTTP/1.0\r\n\r\n' | $HTTPD_CGI 2>/dev/null | grep -Fq 'Location: http://example.com/froot' && pass || fail



H "Timeouts"

title "Read timeout"
(sleep 2.1; printf 'GET / HTTP/1.0\r\n\r\n') | $HTTPD 2>/dev/null | grep -q '.' && fail || pass



H "fnord bugs"

# 1. Should return directory listing of /; instead segfaults
title "Directory indexing of /"
printf 'GET / HTTP/1.0\r\nHost: empty\r\n\r\n' | $HTTPD_IDX 2>/dev/null | grep -q 200 && pass || fail

# 2. Should output \r\n\r\n; instead outputs \r\n\n
title "CGI output bare newlines"
printf 'GET /a.cgi HTTP/1.0\r\n\r\n' | $HTTPD_CGI 2>/dev/null | d | grep -q '#%#%' && pass || fail

## Note: fnord gets a pass on this since it only claims to be an HTTP/1.0
## server.  Eris is not 1.1 compliant either, but it at least tries to fake it.  You
## should consider how much of HTTP/1.1 you want before deploying either.  In practice,
## with browsers, both seems sufficient.  Some tools, notably httperf, fail
## with fnord.
# 3. Should process both requests; instead drops second
title "Multiple requests in one packet"
printf 'GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n' | $HTTPD 2>/dev/null | grep -c '^HTTP/1.' | grep -q 2 && pass || fail

## Skip: eris ignores Accept header (fnord does too, as this bug demonstrates)
# 4. Should return 406 Not Acceptable; instead ignores Accept header
#title "Accept header"
#printf 'GET / HTTP/1.0\r\nAccept: nothing\r\n\r\n' | $HTTPD 2>/dev/null | grep 406 && pass || fail

# 5. Should serve second request as default MIME-Type (text/plain); instead uses previous mime type
title "Second MIME-Type"
(printf 'GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n'
 ls / > /dev/null    # Delay required to work around test #3
 printf 'GET /a HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n') | $HTTPD 2>/dev/null | grep -q 'text/plain\|application/octet-stream' && pass || fail

## Skip: eris doesn't allow POST to static HTML
# 6. Should consume POST data; instead tries to read POST data as second request
#title "POST to static HTML"
#(printf 'POST / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\nContent-Type: text/plain\r\nContent-Length: 1\r\n\r\n';
# ls / > /dev/null
# printf 'aPOST / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\nContent-Type: text/plain\r\nContent-Length: 1\r\n\r\na') | $HTTPD 2>/dev/null | grep -c '200 OK' | grep -q 2 && pass || fail

# 7. HTTP/1.1 should default to keepalive; instead connection is closed
title "HTTP/1.1 default keepalive"
(printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\n'
 ls / >/dev/null
 printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\n') | $HTTPD 2>/dev/null | grep -c '^HTTP/' | grep -q 2 && pass || fail

# 8. Should parse "Thursday"; instead assumes all day names are 6 characters long
title "RFC 850 Date"
printf 'GET / HTTP/1.0\r\nIf-Modified-Since: Thursday, 27-Feb-30 12:12:12 GMT\r\n\r\n' | $HTTPD 2>/dev/null | grep -q '304 Not Changed' && pass || fail

# 9. Should set PATH_INFO to /; instead sets it to /index.html
title "PATH_INFO=/"
printf 'GET /a.cgi/ HTTP/1.0\r\n\r\n' | $HTTPD_CGI 2>/dev/null | grep -Eq 'PATH_INFO=.?/.?$' && pass || fail

echo
echo
echo "$successes of $tests tests passed ($failures failed)."

exit $failures
