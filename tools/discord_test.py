"""Kite - DPI strateji testi.

Discord'a farkli yontemlerle tek bir TLS baglantisi acmayi dener ve hangisinin
calistigini yazar. Bilgisayarda hicbir ayari degistirmez.

Kullanim: python3 discord_test.py [site]
"""
import random
import socket
import ssl
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "discord.com"


def resolve_tcp(name, server="1.1.1.1"):
    """DNS over TCP ile A kaydini cozer (operator DNS'ini atlar)."""
    qid = random.randint(0, 0xFFFF)
    q = struct.pack(">HHHHHH", qid, 0x0100, 1, 0, 0, 0)
    for part in name.split("."):
        q += bytes([len(part)]) + part.encode()
    q += b"\x00" + struct.pack(">HH", 1, 1)
    s = socket.create_connection((server, 53), timeout=6)
    s.sendall(struct.pack(">H", len(q)) + q)
    n = struct.unpack(">H", s.recv(2))[0]
    r = b""
    while len(r) < n:
        r += s.recv(n - len(r))
    s.close()
    an = struct.unpack(">H", r[6:8])[0]
    p = len(q)
    for _ in range(an):
        if r[p] & 0xC0 == 0xC0:
            p += 2
        else:
            while r[p]:
                p += r[p] + 1
            p += 1
        typ, _, _, ln = struct.unpack(">HHIH", r[p:p + 10])
        p += 10
        if typ == 1:
            return socket.inet_ntoa(r[p:p + 4])
        p += ln
    raise RuntimeError("A kaydi yok")


def make_hello():
    ctx = ssl.create_default_context()
    inc, out = ssl.MemoryBIO(), ssl.MemoryBIO()
    obj = ctx.wrap_bio(inc, out, server_hostname=HOST)
    try:
        obj.do_handshake()
    except ssl.SSLWantReadError:
        pass
    return obj, inc, out, out.read()


def tlsrec(h, p):
    """Tek TLS kaydini, payload'in p. baytindan iki kayda boler."""
    ct, ver, body = h[0], h[1:3], h[5:]
    a, b = body[:p], body[p:]
    return (bytes([ct]) + ver + struct.pack(">H", len(a)) + a +
            bytes([ct]) + ver + struct.pack(">H", len(b)) + b)


def send_strategy(s, hello, mode):
    i = hello.find(HOST.encode())
    p = i + 2
    if mode == "normal":
        s.sendall(hello)
    elif mode == "split":
        s.send(hello[:p]); time.sleep(0.01); s.sendall(hello[p:])
    elif mode == "disorder":
        s.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, 1)
        s.send(hello[:p]); time.sleep(0.01)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, 64)
        s.sendall(hello[p:])
    elif mode == "gecikmeli":
        s.send(hello[:p]); time.sleep(1.0); s.sendall(hello[p:])
    elif mode == "bayt-bayt":
        end = i + len(HOST) + 2
        for k in range(end):
            s.send(hello[k:k + 1]); time.sleep(0.002)
        s.sendall(hello[end:])
    elif mode == "tlsrec":
        s.sendall(tlsrec(hello, i - 5 + 2))
    elif mode == "tlsrec+split":
        r = tlsrec(hello, i - 5 + 2)
        s.send(r[:i + 2]); time.sleep(0.01); s.sendall(r[i + 2:])
    elif mode == "tlsrec-bas":
        s.sendall(tlsrec(hello, 1))
    elif mode == "buyuk-harf":
        s.sendall(hello.replace(HOST.encode(), HOST.upper().encode()))


def run(ip, mode):
    obj, inc, out, hello = make_hello()
    s = socket.create_connection((ip, 443), timeout=8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        send_strategy(s, hello, mode)
        while True:
            data = s.recv(65536)
            if not data:
                return "CALISMADI (baglanti kapandi)"
            inc.write(data)
            try:
                obj.do_handshake()
                return "OK  <-- CALISIYOR"
            except ssl.SSLWantReadError:
                o = out.read()
                if o:
                    s.sendall(o)
    except ssl.SSLError as e:
        return f"CALISMADI (TLS hatasi: {e.reason})"
    except Exception as e:
        return f"CALISMADI ({type(e).__name__})"
    finally:
        s.close()


def main():
    ip = resolve_tcp(HOST)
    print(f"{HOST} gercek IP: {ip}\n")
    modes = ["normal", "split", "disorder", "gecikmeli", "bayt-bayt",
             "tlsrec", "tlsrec+split", "tlsrec-bas", "buyuk-harf"]
    for m in modes:
        print(f"{m:14} {run(ip, m)}", flush=True)


if __name__ == "__main__":
    main()
