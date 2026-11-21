"""Smoke test of the Medtrum base identity from a Mac: find its advertisement,
print exactly what a real scanner sees, then connect and check the GATT service.

    python3 tools/medcheck.py [seconds] [name-or-address-substring]

Needs bleak (`pip3 install bleak`). With no argument it looks for the name "MT"
for 12 seconds. Every advertisement from the board's address is printed, so a
failure says whether the set is empty or carrying something unexpected.

TEST EQUIPMENT ONLY: it delivers nothing.
"""
import asyncio, sys

from bleak import BleakClient, BleakScanner

SVC = "669a9001-0008-968f-e311-6050405558b3"


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    want = sys.argv[2] if len(sys.argv) > 2 else "MT"
    seen = {}

    def cb(d, ad):
        name = ad.local_name or d.name or ""
        if name == want or want.lower() in d.address.lower() or name:
            seen[d.address] = (name, ad.service_uuids, ad.manufacturer_data, ad.rssi)

    print(f"scanning {seconds:g}s for {want!r} ...")
    async with BleakScanner(cb):
        await asyncio.sleep(seconds)

    for a, v in seen.items():
        print("ADV", a, v)
    hits = [a for a, v in seen.items() if v[0] == want]
    if not hits:
        print(f"FAIL no advertisement named {want!r} in {len(seen)} device(s) seen")
        for a, v in seen.items():
            print("  saw", a, v[0] or "(no name)", v[3], "dBm")
        sys.exit(1)

    print("PASS advertisement found:", hits)
    addr = hits[0]
    async with BleakClient(addr, timeout=20) as c:
        uu = [s.uuid for s in c.services]
        print("services:", uu)
        print("PASS base service present" if SVC in uu else f"FAIL no base service ({SVC} missing)")


asyncio.run(main())
