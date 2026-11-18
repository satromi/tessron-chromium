#!/usr/bin/env python3
#
# wt_echo_server.py -- a WebTransport echo server for the browser's
# WebTransport (design 14.5), run on the host for the QEMU machine
#
# This software is distributed under the MIT License (LICENSE).
#
#   wt_echo_server.py ADDRESS
#
# ADDRESS is the host's as the guest reaches it under the user mode
# network (WSL: its own address). Needs aioquic (pip3 install --user
# aioquic). The page is then opened with
#
#   make ... NET=user KT_SITE='http://ADDRESS:8790/wt.html\nsecure=http://ADDRESS:8790'
#
# and writes what it did to the console ("wt ready bidi=bidi-hello
# uni=uni-hello dgram=dgram closed").
#
# UDP 4433: HTTP/3 with WebTransport at /echo -- a bidirectional stream's
# bytes come back on it, a unidirectional stream's on a new one from the
# server, a datagram as a datagram. The certificate is ECDSA P-256 for ten
# days, so the page names it by its SHA-256 (serverCertificateHashes).
# TCP 8790: /wt.html, the page, with the hash and the address in it.
import asyncio
import datetime
import hashlib
import http.server
import sys
import threading

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

from aioquic.asyncio import serve
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DatagramReceived, HeadersReceived, WebTransportStreamDataReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ProtocolNegotiated

HOST = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
DIR = '/tmp/wt_echo_server'


def make_cert():
    import os
    os.makedirs(DIR, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'wt-test')])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(hours=1))
            .not_valid_after(now + datetime.timedelta(days=10))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName('wt-test')]), critical=False)
            .sign(key, hashes.SHA256()))
    open(DIR + '/cert.pem', 'wb').write(cert.public_bytes(serialization.Encoding.PEM))
    open(DIR + '/key.pem', 'wb').write(key.private_bytes(serialization.Encoding.PEM,
                                                         serialization.PrivateFormat.PKCS8,
                                                         serialization.NoEncryption()))
    return hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).digest()


class Echo(QuicConnectionProtocol):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.http = None
        self.uni = {}

    def quic_event_received(self, event):
        if isinstance(event, ProtocolNegotiated):
            self.http = H3Connection(self._quic, enable_webtransport=True)
        if self.http is not None:
            for e in self.http.handle_event(event):
                self.h3(e)

    def h3(self, e):
        if isinstance(e, HeadersReceived):
            h = dict(e.headers)
            if h.get(b':method') == b'CONNECT' and h.get(b':protocol') == b'webtransport':
                print('wt: session on', e.stream_id, h.get(b':path'), flush=True)
                self.http.send_headers(e.stream_id, [(b':status', b'200'),
                                                     (b'sec-webtransport-http3-draft', b'draft02')])
            else:
                self.http.send_headers(e.stream_id, [(b':status', b'404')], end_stream=True)
        elif isinstance(e, DatagramReceived):
            print('wt: datagram', e.data, flush=True)
            self.http.send_datagram(e.stream_id, e.data)
        elif isinstance(e, WebTransportStreamDataReceived):
            print('wt: stream', e.stream_id, e.data, e.stream_ended, flush=True)
            if e.stream_id % 4 == 0:            # the client's bidirectional
                self._quic.send_stream_data(e.stream_id, e.data, end_stream=e.stream_ended)
            else:                               # the client's unidirectional
                self.uni[e.stream_id] = self.uni.get(e.stream_id, b'') + e.data
                if e.stream_ended:
                    s = self.http.create_webtransport_stream(e.session_id, is_unidirectional=True)
                    self._quic.send_stream_data(s, self.uni.pop(e.stream_id), end_stream=True)
        self.transmit()


PAGE = '''<!DOCTYPE html><html><head><title>wt</title></head><body><script>
var r = [];
function done() { document.title = 'wt ' + r.join(' '); console.log(document.title); }
var push0 = r.push; r.push = function (x) { push0.call(r, x); done(); };
async function readAll(rs) { var rd = rs.getReader(), out = ''; for (;;) { var x = await rd.read();
 if (x.done) return out; out += new TextDecoder().decode(x.value); } }
async function run() { try {
 var t = new WebTransport('https://HOST:4433/echo', { serverCertificateHashes: [{ algorithm: 'sha-256',
  value: new Uint8Array([HASH]) }] });
 await t.ready; r.push('ready');
 var b = await t.createBidirectionalStream(); var w = b.writable.getWriter();
 await w.write(new TextEncoder().encode('bidi-hello')); await w.close();
 r.push('bidi=' + await readAll(b.readable));
 var u = await t.createUnidirectionalStream(); var uw = u.getWriter();
 await uw.write(new TextEncoder().encode('uni-hello')); await uw.close();
 var inc = await t.incomingUnidirectionalStreams.getReader().read();
 r.push('uni=' + await readAll(inc.value));
 var dw = t.datagrams.writable.getWriter(); await dw.write(new TextEncoder().encode('dgram'));
 var dr = t.datagrams.readable.getReader();
 var d = await Promise.race([dr.read(), new Promise(function (ok) { setTimeout(function () { ok(null); }, 3000); })]);
 r.push('dgram=' + (d ? new TextDecoder().decode(d.value) : 'none'));
 t.close({ closeCode: 7, reason: 'bye' }); await t.closed; r.push('closed');
} catch (e) { r.push('err=' + e.name + ':' + e.message); } done(); }
run();
</script></body></html>'''


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = PAGE.replace('HOST', HOST).replace('HASH', ','.join(str(x) for x in DIGEST)).encode()
        self.send_response(200 if self.path == '/wt.html' else 404)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


DIGEST = make_cert()
threading.Thread(target=lambda: http.server.ThreadingHTTPServer(('0.0.0.0', 8790), Page).serve_forever(),
                 daemon=True).start()
config = QuicConfiguration(alpn_protocols=H3_ALPN, is_client=False, max_datagram_frame_size=65536)
config.load_cert_chain(DIR + '/cert.pem', DIR + '/key.pem')
print('wt: serving on', HOST, 'hash', DIGEST.hex(), flush=True)
loop = asyncio.new_event_loop()
loop.run_until_complete(serve('0.0.0.0', 4433, configuration=config, create_protocol=Echo))
loop.run_forever()
