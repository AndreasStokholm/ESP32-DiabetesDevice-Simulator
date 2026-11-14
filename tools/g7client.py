"""A test central that speaks G7SensorKit's side of the G7 protocol, to check
the ESP32 emulator end to end over real Bluetooth from the Mac.

Fresh pairing (J-PAKE, AES challenge, certificate exchange, key challenge,
finalize), then a session (4e, 59 backfill, 52, 4a, 32), then a reconnect with
the stored key. The J-PAKE client is an independent pure-Python port of
g7sim's jpake_test.go client (which mirrors G7SensorKit's G7JPAKE).
"""
import asyncio, hashlib, os, struct, sys, time
from bleak import BleakScanner, BleakClient
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# P-256
P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
A = -3
G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
     0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)

def add(p, q):
    if p is None: return q
    if q is None: return p
    if p[0] == q[0] and (p[1] + q[1]) % P == 0: return None
    if p == q: l = (3 * p[0] * p[0] + A) * pow(2 * p[1], -1, P) % P
    else: l = (q[1] - p[1]) * pow(q[0] - p[0], -1, P) % P
    x = (l * l - p[0] - q[0]) % P
    return (x, (l * (p[0] - x) - p[1]) % P)

def mul(k, p):
    r = None
    while k:
        if k & 1: r = add(r, p)
        p = add(p, p); k >>= 1
    return r

def neg(p): return (p[0], (-p[1]) % P)
def enc(p): return b"\x04" + p[0].to_bytes(32, "big") + p[1].to_bytes(32, "big")
def i2b(v): return v.to_bytes(32, "big")
def rand(): return int.from_bytes(os.urandom(32), "big") % (N - 2) + 1

SENSOR_PARTY = bytes([0x37, 0x56, 0x27, 0x67, 0x56, 0x27])
CLIENT_PARTY = b"client"

def thash(base, proof, pub, party):
    h = hashlib.sha256()
    for part in (enc(base), enc(proof), enc(pub), party):
        h.update(struct.pack(">I", len(part))); h.update(part)
    return int.from_bytes(h.digest(), "big") % N

def make_cert(base, priv, party):
    v = rand(); proof = mul(v, base); pub = mul(priv, base)
    c = thash(base, proof, pub, party)
    r = (v - c * priv) % N
    return i2b(pub[0]) + i2b(pub[1]) + i2b(proof[0]) + i2b(proof[1]) + i2b(r)

def decode(b):
    n = [int.from_bytes(b[i*32:i*32+32], "big") for i in range(5)]
    return (n[0], n[1]), (n[2], n[3]), n[4]

def verify(base, b, party):
    pub, proof, r = decode(b)
    return add(mul(r, base), mul(thash(base, proof, pub, party), pub)) == proof

def aes_challenge(ch, key):
    e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return (e.update(ch + ch) + e.finalize())[:8]

U = lambda s: f"f808{s}-849e-531c-c594-30f1f86a4ea5"
AUTH, CONTROL, BACKFILL, CERT = U("3535"), U("3534"), U("3536"), U("3538")
NAMES = {AUTH: "auth", CONTROL: "control", BACKFILL: "backfill", CERT: "cert"}

class Session:
    def __init__(self, client):
        self.c = client
        self.q = {AUTH: asyncio.Queue(), CONTROL: asyncio.Queue(), BACKFILL: asyncio.Queue()}
        self.certbuf = bytearray(); self.certev = asyncio.Event()

    async def start(self):
        for u in (CERT, AUTH, CONTROL, BACKFILL):
            await self.c.start_notify(u, self._cb(u))

    def _cb(self, u):
        def f(_, data):
            data = bytes(data)
            if u == CERT:
                self.certbuf += data; self.certev.set()
            else:
                self.q[u].put_nowait(data)
        return f

    async def write(self, u, data):
        print(f"  {NAMES[u]:8} <- {data.hex()}")
        await self.c.write_with_response(u, data) if hasattr(self.c, "write_with_response") else await self.c.write_gatt_char(u, data, response=True)

    async def read(self, u, timeout=20):
        d = await asyncio.wait_for(self.q[u].get(), timeout)
        print(f"  {NAMES[u]:8} -> {d.hex()}")
        return d

    async def read_cert(self, n, timeout=20):
        end = time.time() + timeout
        while len(self.certbuf) < n:
            self.certev.clear()
            await asyncio.wait_for(self.certev.wait(), max(0.01, end - time.time()))
        out = bytes(self.certbuf[:n]); del self.certbuf[:n]
        print(f"  cert     -> {n} bytes")
        return out

    async def send_cert(self, data):
        for i in range(0, len(data), 20):
            await self.c.write_gatt_char(CERT, data[i:i+20], response=True)
        print(f"  cert     <- {len(data)} bytes")

async def find(prefix):
    print(f"scanning for {prefix}")
    d = await BleakScanner.find_device_by_filter(lambda d, ad: (ad.local_name or d.name or "") == prefix, timeout=330)
    if not d: sys.exit("not found")
    print(f"found {d.name} {d.address}")
    return d

def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond: raise SystemExit(1)

async def pair(dev, code):
    pin = int.from_bytes(code.encode(), "big")
    async with BleakClient(dev, timeout=30) as c:
        s = Session(c); await s.start()
        x1, x2 = rand(), rand()
        t0 = time.time()
        # Round 0 and 1: the sensor streams its cert, the client sends its own.
        await s.write(AUTH, bytes([0x0A, 0x00]))
        s1 = await s.read_cert(160)
        check(verify(G, s1, SENSOR_PARTY), "sensor round 1 Schnorr proof verifies")
        await s.send_cert(make_cert(G, x1, CLIENT_PARTY))
        await s.write(AUTH, bytes([0x0A, 0x01]))
        s2 = await s.read_cert(160)
        check(verify(G, s2, SENSOR_PARTY), "sensor round 2 Schnorr proof verifies")
        await s.send_cert(make_cert(G, x2, CLIENT_PARTY))
        S1, S2 = decode(s1)[0], decode(s2)[0]
        await s.write(AUTH, bytes([0x0A, 0x02]))
        s3 = await s.read_cert(160)
        base3 = add(add(mul(x1, G), mul(x2, G)), S1)
        check(verify(base3, s3, SENSOR_PARTY), "sensor round 3 proof verifies over X1+X2+X3")
        bk = x2 * pin % N
        await s.send_cert(make_cert(add(add(mul(x1, G), S1), S2), bk, CLIENT_PARTY))
        K = mul(x2, add(decode(s3)[0], neg(mul(bk, S2))))
        key = hashlib.sha256(i2b(K[0])).digest()[:16]
        print(f"  J-PAKE done in {time.time()-t0:.1f}s, key {key.hex()}")

        # AES challenge.
        cc = os.urandom(8)
        await s.write(AUTH, bytes([0x02]) + cc + bytes([0x02]))
        r = await s.read(AUTH)
        check(r[0] == 0x03 and r[1:9] == aes_challenge(cc, key), "sensor answered our challenge under the J-PAKE key")
        await s.write(AUTH, bytes([0x04]) + aes_challenge(r[9:17], key))
        r = await s.read(AUTH)
        check(r == bytes([0x05, 0x01, 0x00]), "auth status 05 01 00 (paired)")

        # Certificate exchange.
        certs = []
        for idx in (0, 1):
            mine = os.urandom(300)
            await s.write(AUTH, bytes([0x0B, idx]) + struct.pack("<I", len(mine)))
            ack = await s.read(AUTH)
            n = struct.unpack("<I", ack[3:7])[0]
            certs.append(await s.read_cert(n))
            await s.send_cert(mine)
        await s.write(AUTH, bytes([0x0B, 0x02, 0, 0, 0, 0]))
        check((await s.read(AUTH))[:3] == bytes([0x0B, 0x00, 0x02]), "certificate terminator acknowledged")
        ca = x509.load_der_x509_certificate(certs[0]); leaf = x509.load_der_x509_certificate(certs[1])
        print(f"  CA {ca.subject.rfc4514_string()}, leaf {leaf.subject.rfc4514_string()}")
        ca.public_key().verify(leaf.signature, leaf.tbs_certificate_bytes, ec.ECDSA(hashes.SHA256()))
        check(True, "leaf certificate is signed by the dummy CA")

        # Key challenge: the sensor signs our nonce with its leaf key.
        nonce = os.urandom(16)
        await s.write(AUTH, bytes([0x0C]) + nonce)
        ack = await s.read(AUTH)
        sig = await s.read_cert(64)
        der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
        leaf.public_key().verify(der, nonce, ec.ECDSA(hashes.SHA256()))
        check(True, "sensor's key-challenge signature verifies with its leaf key")
        await s.send_cert(os.urandom(64))  # we do not hold Dexcom's display key

        # Finalize.
        await s.write(AUTH, bytes([0x06]))
        check(await s.read(AUTH) == bytes([0x06, 0x1E]), "finalize 06 1E")
        await s.write(AUTH, bytes([0x07]))
        check(await s.read(AUTH) == bytes([0x07, 0x00]), "bond request 07 00")
        await s.read(AUTH)  # 08 00

        await session(s)
        return key

async def session(s):
    await s.write(CONTROL, bytes([0x4E]))
    g = await s.read(CONTROL)
    now, idx = struct.unpack("<IH", g[2:8]); age, glucose = struct.unpack("<HH", g[10:14])
    check(g[0] == 0x4E, f"glucose: {glucose & 0xfff} mg/dL, state {g[14]}, trend {struct.unpack('b', g[15:16])[0]/10}, reading {idx}, sensor age {now}s, reading {age}s old")
    await s.write(CONTROL, bytes([0x59]) + struct.pack("<II", max(0, now - 3600), now))
    recs = []
    while True:
        try:
            b = await asyncio.wait_for(s.q[BACKFILL].get(), 0.1)
            recs += [b[i:i+9] for i in range(0, len(b), 9)]
            continue
        except asyncio.TimeoutError:
            pass
        if not s.q[CONTROL].empty():
            break
    end = await s.read(CONTROL)
    while not s.q[BACKFILL].empty():
        b = s.q[BACKFILL].get_nowait(); recs += [b[i:i+9] for i in range(0, len(b), 9)]
    vals = [struct.unpack("<H", r[4:6])[0] & 0xfff for r in recs]
    check(end == bytes([0x59, 0x00]) and len(recs) >= 11, f"backfill: {len(recs)} records {vals}")
    await s.write(CONTROL, bytes([0x52]))
    v = await s.read(CONTROL)
    check(v[0] == 0x52 and len(v) == 15, f"extended version: session {struct.unpack('<I', v[2:6])[0]}s warmup {struct.unpack('<H', v[6:8])[0]}s")
    await s.write(CONTROL, bytes([0x4A]))
    check((await s.read(CONTROL))[0] == 0x4A, "transmitter version")
    await s.write(CONTROL, bytes([0x32]))
    check((await s.read(CONTROL))[0] == 0x32, "calibration bounds")

async def reconnect(dev, key):
    async with BleakClient(dev, timeout=30) as c:
        s = Session(c); await s.start()
        cc = os.urandom(8)
        await s.write(AUTH, bytes([0x02]) + cc + bytes([0x03]))  # as a watch: the phone already collected
        r = await s.read(AUTH)
        check(r[1:9] == aes_challenge(cc, key), "reconnect: sensor proves the stored key")
        await s.write(AUTH, bytes([0x04]) + aes_challenge(r[9:17], key))
        # A watch has no slot: the sensor refuses it with no app key.
        r = await s.read(AUTH)
        print(f"  watch without a slot -> {r.hex()}")

async def reconnect_phone(dev, key):
    async with BleakClient(dev, timeout=30) as c:
        s = Session(c); await s.start()
        cc = os.urandom(8)
        await s.write(AUTH, bytes([0x02]) + cc + bytes([0x02]))
        r = await s.read(AUTH, timeout=30)
        if r[0] != 0x03:
            raise SystemExit(f"unexpected {r.hex()}")
        check(r[1:9] == aes_challenge(cc, key), "reconnect: sensor proves the stored key")
        await s.write(AUTH, bytes([0x04]) + aes_challenge(r[9:17], key))
        check(await s.read(AUTH) == bytes([0x05, 0x01, 0x01]), "reconnect: auth status 05 01 01 (bonded)")
        await session(s)

async def main():
    dev = await find(os.environ["G7NAME"])
    if sys.argv[1] == "reconnect":
        await reconnect_phone(dev, bytes.fromhex(sys.argv[2]))
        print("ALL RECONNECT CHECKS PASSED")
        return
    key = await pair(dev, sys.argv[1])
    print("ALL PAIRING CHECKS PASSED")
    print("key", key.hex())

if __name__ == "__main__":
    asyncio.run(main())
