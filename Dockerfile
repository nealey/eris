FROM alpine

RUN apk --no-cache add s6-networking

RUN apk --no-cache add build-base
COPY . /usr/local/src/eris
RUN make -C /usr/local/src/eris
RUN cp /usr/local/src/eris/eris /usr/bin
RUN rm -rf /usr/local/src/eris
RUN apk --no-cache del build-base

RUN addgroup -S -g 800 www
RUN adduser -S -u 800 -G www www

RUN mkdir /www
WORKDIR /www

EXPOSE 80

CMD ["s6-tcpserver", "-u", "80", "-g", "80", "0.0.0.0", "80", "/usr/bin/eris", "-."]

