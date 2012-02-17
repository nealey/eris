#! /usr/bin/python3

import unittest
from subprocess import *
import os

def eris(*args):
    return Popen(('../eris',) + args, 
                 stdin=PIPE, stdout=PIPE, stderr=PIPE,
                 env={'PROTO': 'TCP',
                      'TCPREMOTEPORT': '5858',
                      'TCPREMOTEIP': '10.1.2.3'})

class LinesTests(unittest.TestCase):
    def assertLinesEqual(self, a, b):
        self.assertSequenceEqual(a.split(b'\n'), b.split(b'\n'))

class ArgTests(LinesTests):
    def check_index(self, *args):
        p = eris(*args)
        so, se = p.communicate(b'GET / HTTP/1.0\r\n\r\n')
        self.assertRegexpMatches(so, b'HTTP/1.0 200 OK\r\nServer: eris/2\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: 6\r\nLast-Modified: (Mon|Tue|Wed|Thu|Fri|Sat|Sun), .. (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) 2... ..:..:.. GMT\r\n\r\njames\n')
        self.assertLinesEqual(se, b'10.1.2.3 200 6 127.0.0.1 (null) (null) /index.html\n')

    def testArgs(self):
        "Make sure index.html is the same for all arguments"

        self.check_index()
        self.check_index('-d')
        self.check_index('-r')
        self.check_index('-c')

    def testPortAppend(self):
        p = eris('-p')
        so, se = p.communicate(b'GET / HTTP/1.0\r\n\r\n')
        self.assertRegexpMatches(so, b'HTTP/1.0 200 OK\r\nServer: eris/2\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: 6\r\nLast-Modified: (Mon|Tue|Wed|Thu|Fri|Sat|Sun), .. (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) 2... ..:..:.. GMT\r\n\r\njames\n')
        self.assertLinesEqual(se, b'10.1.2.3 200 6 127.0.0.1:80 (null) (null) /index.html\n')

    def testBadArgs(self):
        "Make sure bad arguments actually fail"

        self.assertRaises(AssertionError, self.check_index, '-Z')


class BasicTests(LinesTests):
    args = []

    def setUp(self):
        self.p = eris(*self.args)
        
    def tearDown(self):
        del self.p

    def get(self, path, host):
        h = 'GET %s HTTP/1.0\r\nHost: %s\r\n\r\n' % (path, host)
        so, se = self.p.communicate(h.encode('utf-8'))
        return (so, se)

    def post(self, path, host, formdata):
        h = 'POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n%s' % (path, host, len(formdata), formdata)
        so, se = self.p.communicate(h.encode('utf-8'))
        return (so, se)

class DirTests(BasicTests):
    args = ['-d']

    def testRootDir(self):
        so, se = self.get('/', 'empty')
        self.assertLinesEqual(so, b'HTTP/1.0 200 OK\r\nServer: eris/2\r\nConnection: close\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<h3>Directory Listing: /</h3>\n<pre>\n</pre>\n')
        self.assertLinesEqual(se, b'10.1.2.3 200 32 empty (null) (null) /\n')

    def testNoTrailingSlash(self):
        so, se = self.get('/files', 'default')
        self.assertLinesEqual(so, b'HTTP/1.0 404 Not Found\r\nConnection: close\r\nContent-Length: 50\r\nContent-Type: text/html\r\n\r\n<title>Not Found</title>No such file or directory.')
        self.assertLinesEqual(se, b'10.1.2.3 404 0 default (null) (null) /files\n')

    def testFiles(self):
        so, se = self.get('/files/', 'default')

        self.assertLinesEqual(so, b'HTTP/1.0 200 OK\r\nServer: eris/2\r\nConnection: close\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<h3>Directory Listing: /files/</h3>\n<pre>\n<a href="/">Parent directory</a>\n[TXT] <a href="1.txt">1.txt</a>\n</pre>\n')
        self.assertLinesEqual(se, b'10.1.2.3 200 110 default (null) (null) /files/\n')


class CGITests(BasicTests):
    args = ['-c']
    maxDiff = None

    def testSet(self):
        so, se = self.get('/cgi/set.cgi', 'default')
        self.assertLinesEqual(so, b'HTTP/1.0 200 OK\r\nServer: eris/2\r\nPragma: no-cache\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nGATEWAY_INTERFACE:CGI/1.1\nSERVER_PROTOCOL:HTTP/1.0\nSERVER_SOFTWARE:eris/2\nSERVER_NAME:default\nSERVER_PORT:80\nREQUEST_METHOD:GET\nREQUEST_URI:/cgi/set.cgi\nSCRIPT_NAME:/cgi/set.cgi\nREMOTE_ADDR:10.1.2.3\nREMOTE_PORT:5858\n')
        self.assertLinesEqual(se, b'10.1.2.3 200 242 default (null) (null) /cgi/set.cgi\n')

    def testSetArgs(self):
        so, se = self.get('/cgi/set.cgi?a=1&b=2&c=3', 'default')
        self.assertLinesEqual(so, b'HTTP/1.0 200 OK\r\nServer: eris/2\r\nPragma: no-cache\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nGATEWAY_INTERFACE:CGI/1.1\nSERVER_PROTOCOL:HTTP/1.0\nSERVER_SOFTWARE:eris/2\nSERVER_NAME:default\nSERVER_PORT:80\nREQUEST_METHOD:GET\nREQUEST_URI:/cgi/set.cgi\nSCRIPT_NAME:/cgi/set.cgi\nREMOTE_ADDR:10.1.2.3\nREMOTE_PORT:5858\nQUERY_STRING:a=1&b=2&c=3\n')
        self.assertLinesEqual(se, b'10.1.2.3 200 267 default (null) (null) /cgi/set.cgi\n')

    def testPost(self):
        so, se = self.post('/cgi/set.cgi', 'default', 'a=1&b=2&c=3')
        self.assertLinesEqual(se, b'10.1.2.3 200 330 default (null) (null) /cgi/set.cgi\n')
        self.assertLinesEqual(so, b'HTTP/1.0 200 OK\r\nServer: eris/2\r\nPragma: no-cache\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nGATEWAY_INTERFACE:CGI/1.1\nSERVER_PROTOCOL:HTTP/1.0\nSERVER_SOFTWARE:eris/2\nSERVER_NAME:default\nSERVER_PORT:80\nREQUEST_METHOD:POST\nREQUEST_URI:/cgi/set.cgi\nSCRIPT_NAME:/cgi/set.cgi\nREMOTE_ADDR:10.1.2.3\nREMOTE_PORT:5858\nCONTENT_TYPE:application/x-www-form-urlencoded\nCONTENT_LENGTH:11\nForm data: a=1&b=2&c=3')

# XXX: Test posting to static html with keepalive
# (it probably won't discard content-length octets)

unittest.main()

