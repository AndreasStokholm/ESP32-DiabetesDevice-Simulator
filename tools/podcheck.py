"""Smoke test of the pod identity from a Mac: find the pod's advert, connect,
check its GATT service, and do the first link-layer steps (hello, then RTS -> CTS)."""
import asyncio, sys
from bleak import BleakScanner, BleakClient
SVC = "1a7e4024-e3ed-4464-8b7e-751e03d0dc5f"
CMD = "1a7e2441-e3ed-4464-8b7e-751e03d0dc5f"
async def main():
    seen = {}
    def cb(d, ad):
        n = ad.local_name or d.name or ""
        if "Fake POD" in n or n.startswith("DXCMoo"):
            seen[d.address] = (n, ad.service_uuids, ad.manufacturer_data, ad.rssi)
    async with BleakScanner(cb):
        await asyncio.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 12)
    for a, v in seen.items(): print("ADV", a, v)
    pod = [a for a, v in seen.items() if "Fake POD" in v[0]]
    if not pod: sys.exit("FAIL pod not seen")
    got = asyncio.Queue()
    async with BleakClient(pod[0], timeout=20) as c:
        uu = [s.uuid for s in c.services]
        print("services:", uu)
        print("PASS pod service present" if SVC in uu else "FAIL no pod service")
        await c.start_notify(CMD, lambda _, d: got.put_nowait(bytes(d)))
        await c.write_gatt_char(CMD, bytes([0x06, 0x01, 0x04, 0x00, 0x00, 0x10, 0x00]), response=False)  # hello, as OmnipodKit
        await asyncio.sleep(0.3)
        await c.write_gatt_char(CMD, bytes([0x00]), response=False)   # RTS
        r = await asyncio.wait_for(got.get(), 5)
        print("PASS RTS -> CTS" if r == bytes([0x01]) else f"FAIL got {r.hex()}")
asyncio.run(main())
