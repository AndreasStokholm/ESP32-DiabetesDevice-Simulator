# ESP-Sim + G7

One ESP32-S3 that is both an **Omnipod pod simulator** (DASH or Omnipod 5) and a
**Dexcom G7 sensor simulator**. Pair both from Loop or Trio on a test phone and the app has
a pump and a CGM, with no real pod or sensor.

> **Test equipment only.** Not a Dexcom or Insulet product, and not a medical device. It
> delivers no insulin and every value it reports is made up. Never use it, or an app
> paired to it, for anyone's therapy.

It combines two projects:

- the pod is [nightscout/ESP-Sim](https://github.com/nightscout/ESP-Sim)'s pod core,
  unmodified, in `lib/dashpod` (commit `26c5219`);
- the G7 is [g7simESP32](https://github.com/loudestnoise/g7simESP32), a C++ port of
  [LoopKit/g7sim](https://github.com/LoopKit/g7sim).

## How one board is two devices

A phone can't hold two Bluetooth connections to the same radio, so the board can't be two
separate devices to one phone. Instead it is **one Bluetooth device** that carries both the
pod's service and the G7's. The app's pod code and its G7 code each connect to "their"
device and share the single connection underneath.

- **Advertising** takes turns between the pod's advertisement (` :: Fake POD ::`) and the
  G7's (`DXCMxx`), depending on which needs to be found.
- **Every 5 minutes** the board hangs up once, when the pod is quiet. A real G7 hangs up
  after each reading and the app only asks for a reading after connecting, so this is what
  makes it collect the new one. Both sides reconnect within a second; the reading can
  arrive up to about 30 seconds late.
- **One phone at a time.**

## Flash from a browser

The easiest way: open **https://loudestnoi.se/esp-sim/** in Chrome or Edge on a computer,
plug in the board and press *Connect and install*. Nothing to install.

To host the flasher yourself, `tools/make_flasher.sh` builds it into `flasher/dist`
(page, manifest, one merged firmware image, and a pinned copy of ESP Web Tools); put that
folder on any HTTPS server.

## Build and flash from source

```bash
brew install platformio
pio run -t upload        # build and flash
pio device monitor       # the console
```

Tested on an ESP32-S3 with 8 MB flash, with Trio on a test iPhone: a DASH pod and an
Omnipod 5 pod each paired, primed and ran, with the G7 paired on the same connection and
its readings collected every 5 minutes.

## Pair

- **G7:** add a Dexcom G7 in the app and enter the 4-digit pairing code the console prints.
- **Pod:** pair a new Omnipod DASH pod. For Omnipod 5, switch the pod type first (pod
  panel, or `pod type o5`).

Use a **test phone**, never one that runs a real sensor or pod.

## Control

**Web panels.** With no Wi‑Fi network saved, the board runs its own access point,
`ESP-Sim-XXXX`; its password is printed on the console at boot. Join it and open
`http://192.168.4.1/`. To put the board on your network instead, use the pod panel's Wi‑Fi
card or `wifi <name> <password>`; it then prints its address (also `http://esp-sim.local/`)
and takes the time from NTP.

- `/` is the G7 panel: glucose (fixed, sine, random walk), sensor errors, new sensor.
- `/pod` is the pod panel: reservoir, pod age, alerts, faults, DASH or Omnipod 5, new pod.

The JSON APIs are the originals': g7sim's at `/status`, `/glucose`, `/forget`, `/new`, and
ESP-Sim's under `/pod` (`/pod/status`, `/pod/command`, `/pod/wifi`).

**Console** (USB serial; `help` lists everything):

| Command | What it does |
|---|---|
| `status` | Both devices: sensor, pairing code, latest reading, pod state |
| `glucose fixed 55` / `sine 120 50 240` / `walk 120` | Set the glucose model |
| `algo 18` | Report a sensor error (`algo 0` is normal) |
| `forget` / `new code=1234 age=0` | Unpair the sensor's phones / start a new sensor |
| `pod` | The pod's status |
| `pod new` / `pod type dash\|o5` | A new, unpaired pod |
| `pod reservoir 30` / `pod age 4200` / `pod fault 20` / `pod alerts 4` | Pod conditions |
| `pod selftest` | Check the pod's crypto on this board |
| `wifi <name> [password]` / `wifi off` | Join or forget a Wi‑Fi network |

## Good to know

- Both devices keep their state in flash: a reboot is the same sensor and the same pod,
  still paired.
- Without NTP, both clocks pause while the board is unplugged.
- In the app the pod may briefly show as reconnecting once every 5 minutes. That's the
  hang-up described above.
- Unlike a real G7, the sensor serves a phone that has already collected the current
  reading, and it does not serve a second display (a watch or a second phone).
- Neither device appears in iOS Settings → Bluetooth. That's expected.
- Not yet tested: the web panels over the board's own access point, and long runs.
- The ESP32 is 2.4 GHz only, and can't sign in to captive-portal Wi‑Fi (hotels). Use its
  own access point or a phone hotspot.

## Differences from the originals

- **ESP-Sim** is an ESP-IDF project; here its pod core runs under Arduino/PlatformIO with
  new board glue (`src/pod.cpp`, and the pod's side of `src/radio.cpp`) in place of its
  `main/`. The pod's panel moved to `/pod`. ESP-Sim's host tests are not included: run them
  in ESP-Sim itself.
- **g7simESP32**'s radio is rewritten to share one connection with the pod, and its
  session stays open on that connection instead of hanging up after a few idle seconds.

## Layout

| Path | What |
|---|---|
| `lib/dashpod/` | ESP-Sim's pod core, unmodified (GPL-3.0-or-later) |
| `src/pod.cpp` | The pod's clock, storage and task |
| `src/radio.cpp` | One Bluetooth device for both |
| `src/sensor.cpp`, `handler.cpp`, `crypto.cpp` | The G7 (from g7simESP32) |
| `src/main.cpp` | Boot, Wi‑Fi, web panels, console |
| `flasher/`, `tools/make_flasher.sh` | The browser flasher |
| `tools/g7client.py`, `tools/podcheck.py` | Test clients for a Mac |

## License

GPL-3.0-or-later (see `LICENSE`), as ESP-Sim's pod core requires of a combined work. The G7
files come from MIT-licensed code (LoopKit/g7sim and g7simESP32); their notice is in
`LICENSE-g7sim-MIT`.
