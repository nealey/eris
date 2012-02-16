#! /bin/sh

echo Content-Type: text/plain
echo
set | sort | grep 'GATEWAY\|SERVER\|REQUEST\|SCRIPT\|REMOTE\|HTTP\|AUTH\|CONTENT\|QUERY\|PATH_'
if [ -n "$CONTENT_TYPE" ]; then
    echo -n "Form data: "
    dd bs=1 count=$CONTENT_LENGTH 2>/dev/null
fi
