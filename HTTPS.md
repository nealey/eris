SSL with eris
=============

Eris does not care what transport is in use: that job is left to the invoking
program (e.g. tcpserver).

In the past you could use `sslio` with `tcpsvd`,
but `sslio` has not been updated in a long time,
and won't work with (at least) Chrome 39.

I recommend using stunnel,
which also works with IPv6.
You can invoke it like so:

	#! /bin/sh
	cd /srv/www
	HTTPS=enabled; export HTTPS

	exec stunnel -fd 3 3<<EOD
	foreground = yes
	setuid = http
	setgid = http
	debug = 4

	[https]
	accept = ::443
	cert = /path/to/yourserver.crt
	key = /path/to/yourserver.key
	exec = /path/to/eris
	execargs = eris -c
	EOD

I set the `HTTPS` environment variable,
so CGI can tell whether or not its connection is secure.
