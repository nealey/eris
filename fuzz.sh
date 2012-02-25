#! /bin/sh

start () {
    printf "%-20s [" "$@"
}

fin () {
    echo ']'
}

failures=0
fail () {
    echo -n '!'
    failures=$(expr $failures + 1)
}

start "Full fuzz"
for i in $(seq 200); do
    ./eris < /dev/urandom 2>/dev/null | grep -q '^HTTP/1.0 40' || fail
    [ $(expr $i % 5) = 0 ] && echo -n '.'
done
fin

start "Path"
for i in $(seq 200); do
    (
        printf "GET /"
        dd if=/dev/urandom count=2 2>/dev/null | tr -d '\0\r\n ?'
        printf " HTTP/1.0\r\n\r\n"
    ) | ./eris 2>/dev/null | grep -q '^HTTP/1.0 404' || fail
    [ $(expr $i % 5) = 0 ] && echo -n '.'
done
fin

start "Header"
for i in $(seq 200); do
    (
        printf "GET / HTTP/1.0\r\n"
        dd if=/dev/urandom count=2 2>/dev/null | tr -d '\0'
        printf "\r\n\r\n"
    ) | ./eris 2>/dev/null | grep -q '^HTTP/1.0 200' || fail
    [ $(expr $i % 5) = 0 ] && echo -n '.'
done
fin

if [ $failures -eq 0 ]; then
    echo "All tests passed!"
else
    echo "FAIL: $failures failures"
    exit $failures
fi
