#! /bin/sh

echo Content-Type: text/plain
echo
for k in GATEWAY_INTERFACE \
         SERVER_PROTOCOL SERVER_SOFTWARE SERVER_NAME SERVER_PORT \
         REQUEST_METHOD REQUEST_URI \
         SCRIPT_NAME \
         REMOTE_ADDR REMOTE_PORT REMOTE_IDENT \
         HTTP_USER_AGENT HTTP_COOKIE HTTP_REFERER HTTP_ACCEPT_ENCODING \
         AUTH_TYPE \
         CONTENT_TYPE CONTENT_LENGTH \
         QUERY_STRING \
         PATH_INFO PATH_TRANSLATED; do
    v=$(eval echo \${$k})
    if [ -n "$v" ]; then
        echo "$k:$v"
    fi
done
if [ -n "$CONTENT_TYPE" ]; then
    echo -n "Form data: "
    dd bs=1 count=$CONTENT_LENGTH 2>/dev/null
fi
