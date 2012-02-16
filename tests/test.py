#! /usr/bin/python3

import unittest
from subprocess import *
import os

def fnord(*args):
    return Popen(('../fnord',) + args, 
                 stdin=PIPE, stdout=PIPE, stderr=PIPE,
                 env={'PROTO': 'TCP',
                      'TCPREMOTEPORT': '5858',
                      'TCPREMOTEIP': '10.1.2.3'})

class ArgTests(unittest.TestCase):
    def check_index(self, *args):
        p = fnord(*args)
        so, se = p.communicate(b'GET / HTTP/1.0\r\n\r\n')
        self.assertEqual(so, b'HTTP/1.0 200 OK\r\nServer: fnord/2.0\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: 6\r\nLast-Modified: Thu, 16 Feb 2012 04:08:57 GMT\r\n\r\njames\n')
        self.assertEqual(se, b'10.1.2.3 200 6 127.0.0.1:80 (null) (null) /index.html\n')

    def testArgs(self):
        "Make sure index.html is the same for all arguments"

        self.check_index()
        self.check_index('-d')
        self.check_index('-r')
        self.check_index('-c')

    def testBadArgs(self):
        "Make sure bad arguments actually fail"

        self.assertRaises(AssertionError, self.check_index, '-Z')


class BasicTests(unittest.TestCase):
    args = []

    def setUp(self):
        self.p = fnord(*self.args)
        
    def tearDown(self):
        del self.p

    def get(self, path, host):
        h = 'GET %s HTTP/1.0\r\nHost: %s\r\n\r\n' % (path, host)
        so, se = self.p.communicate(h.encode('utf-8'))
        return (so, se)


class DirTests(BasicTests):
    args = ['-d']

    def testRootDir(self):
        so, se = self.get('/', 'moo')
        self.assertEqual(so, b'HTTP/1.0 200 OK\r\nServer: fnord/2.0\r\nConnection: close\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<h3>Directory Listing: /</h3>\n<pre>\n</pre>\n')
        self.assertEqual(se, b'10.1.2.3 200 32 moo:80 (null) (null) /\n')


unittest.main()
