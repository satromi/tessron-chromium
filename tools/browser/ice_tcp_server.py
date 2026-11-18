#!/usr/bin/env python3
#
# ice_tcp_server.py -- a peer that takes ICE over TCP, run on the host for
# the QEMU machine, to try the browser's WebRTC stream sockets (design
# 14.5.2)
#
# This software is distributed under the MIT License (LICENSE).
#
#   ice_tcp_server.py ADDRESS
#
# ADDRESS is the host's as the guest reaches it under the user mode
# network (WSL: its own address). The page is then opened with
#
#   make ... NET=user KT_SITE='http://ADDRESS:8791/ice.html'
#
# and writes what it did to the console ("ice-tcp connected ...").
#
# TCP 8791: /ice.html, the page. It makes an offer with a data channel and
# answers it itself as an ICE-lite peer whose one candidate is a passive
# TCP candidate at ADDRESS:8793, so that the browser, the controlling
# agent, connects to it over TCP (RFC 6544, the packets framed as RFC 4571
# says) and checks the pair with STUN binding requests.
# TCP 8793: the peer. It answers each binding request with a success
# response (XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY with the answer's ice-pwd,
# FINGERPRINT); anything else (DTLS) it reads and leaves.
# /go: a redirection to where go.txt beside this script says (/ice.html
# when there is none), so that one test image made with
# KT_SITE='http://ADDRESS:8791/go' opens one site after another, a run each.
import hashlib
import hmac
import http.server
import os
import socket
import struct
import sys
import threading
import zlib

HOST = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
UFRAG = 'tsufrag'
PWD = 'tessronospasswordforice1'
COOKIE = 0x2112A442

PAGE = r'''<!DOCTYPE html><html><head><title>ice</title></head><body><script>
function wait(ms) { return new Promise(function (ok) { setTimeout(ok, ms); }); }
async function run() {
  var out = 'ice-tcp ';
  try {
    var pc = new RTCPeerConnection();
    pc.createDataChannel('d');
    var o = await pc.createOffer();
    await pc.setLocalDescription(o);
    var mid = /a=mid:(\S+)/.exec(o.sdp)[1];
    var fp = [];
    for (var i = 0; i < 32; i++) fp.push('AB');
    var sdp = 'v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE ' + mid + '\r\na=ice-lite\r\n'
      + 'm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=mid:' + mid + '\r\n'
      + 'a=ice-ufrag:@UFRAG@\r\na=ice-pwd:@PWD@\r\na=fingerprint:sha-256 ' + fp.join(':') + '\r\n'
      + 'a=setup:passive\r\na=sctp-port:5000\r\n'
      + 'a=candidate:1 1 tcp 2130706431 ' + location.hostname + ' 8793 typ host tcptype passive\r\n'
      + 'a=end-of-candidates\r\n';
    await pc.setRemoteDescription({ type: 'answer', sdp: sdp });
    for (var t = 0; t < 75 && pc.iceConnectionState !== 'connected' && pc.iceConnectionState !== 'completed'; t++) await wait(200);
    out += pc.iceConnectionState;
    var rep = await pc.getStats(), loc = {};
    rep.forEach(function (v) { if (v.type === 'local-candidate') loc[v.id] = v; });
    rep.forEach(function (v) {
      if (v.type === 'candidate-pair' && v.state === 'succeeded') {
        var l = loc[v.localCandidateId] || {};
        out += ' pair=' + l.protocol + ',' + (l.tcpType || '') + ',sent' + v.requestsSent + ',got' + v.responsesReceived;
      }
    });
  } catch (e) { out += 'err=' + e.name + ':' + e.message; }
  console.log(out);
  document.title = out;
}
run();
</script></body></html>'''.replace('@UFRAG@', UFRAG).replace('@PWD@', PWD)


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/go':
            # where the file go.txt beside this script says (/ice.html when there is none): one test
            # image (KT_SITE='http://ADDRESS:8791/go') opens other sites run by run
            try:
                to = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'go.txt')).read().strip()
            except OSError:
                to = '/ice.html'
            self.send_response(302)
            self.send_header('Location', to or '/ice.html')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        body = PAGE.encode()
        self.send_response(200 if self.path == '/ice.html' else 404)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def attr(kind, value):
    pad = (4 - len(value) % 4) % 4
    return struct.pack('!HH', kind, len(value)) + value + b'\0' * pad


def answer(req, peer):
    kind, length, cookie = struct.unpack('!HHI', req[:8])
    txid = req[8:20]
    if kind != 0x0001 or cookie != COOKIE:
        return None
    ip = socket.inet_aton(peer[0])
    xaddr = struct.pack('!BBH', 0, 1, peer[1] ^ (COOKIE >> 16)) + bytes(a ^ b for a, b in zip(ip, struct.pack('!I', COOKIE)))
    attrs = attr(0x0020, xaddr)
    # MESSAGE-INTEGRITY over the header (its length counting the attribute) and what is before it
    head = struct.pack('!HHI', 0x0101, len(attrs) + 24, COOKIE) + txid
    mi = hmac.new(PWD.encode(), head + attrs, hashlib.sha1).digest()
    attrs += attr(0x0008, mi)
    head = struct.pack('!HHI', 0x0101, len(attrs) + 8, COOKIE) + txid
    crc = (zlib.crc32(head + attrs) ^ 0x5354554e) & 0xffffffff
    attrs += attr(0x8028, struct.pack('!I', crc))
    return struct.pack('!HHI', 0x0101, len(attrs), COOKIE) + txid + attrs


def peer(conn, addr):
    print('ice: a connection from %s:%d' % addr, flush=True)
    buf = b''
    checks = 0
    try:
        while True:
            got = conn.recv(65536)
            if not got:
                break
            buf += got
            while len(buf) >= 2:
                n = struct.unpack('!H', buf[:2])[0]
                if len(buf) < 2 + n:
                    break
                pkt, buf = buf[2:2 + n], buf[2 + n:]
                if len(pkt) >= 20 and pkt[0] < 2:
                    resp = answer(pkt, addr)
                    if resp:
                        checks += 1
                        if checks <= 3:
                            print('ice: a binding request answered', flush=True)
                        conn.sendall(struct.pack('!H', len(resp)) + resp)
    except OSError:
        pass
    print('ice: the connection closed after %d checks' % checks, flush=True)
    conn.close()


def stun_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', 8793))
    s.listen(8)
    while True:
        conn, addr = s.accept()
        threading.Thread(target=peer, args=(conn, addr), daemon=True).start()


threading.Thread(target=stun_server, daemon=True).start()
print('ice: http://%s:8791/ice.html' % HOST, flush=True)
http.server.ThreadingHTTPServer(('0.0.0.0', 8791), Page).serve_forever()
