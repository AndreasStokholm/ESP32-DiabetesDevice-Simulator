# ESP-Sim + G7

One ESP32-S3 that is both an **Omnipod pod simulator** (DASH or Omnipod 5) and a
**Dexcom G7 sensor simulator**. Pair both from Loop or Trio on a test phone and the app has
a pump and a CGM, with no real pod or sensor.

> **Test equipment only.** Not a Dexcom or Insulet product, and not a medical device. It
> delivers no insulin and every value it reports is made up. Never use it, or an app
> paired to it, for anyone's therapy.

It combines two projects:

- the pod is [nightscout/ESP-Sim](https://github.com/nightscout/ESP-Sim)'s pod core,
  in `lib/dashpod` (commit `26c5219`, plus loopkit/pod's `ignoreNextMessages` and
  `failNextConnects` commands);
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

## Flashing the firmware

You need an **ESP32-S3 board with at least 8 MB of flash** and a USB **data** cable (some
cables only charge). If the board has two USB sockets, use the one marked *USB*, not
*UART* or *COM*.

### From a browser (easiest)

Nothing to install. Needs Chrome or Edge on a computer; Safari, Firefox and phones can't
flash.

1. Open **https://loudestnoi.se/esp-sim/** and plug in the board.
2. Press **Connect and install** and pick the board's port, usually listed as
   *USB JTAG/serial debug unit*.
3. Choose **Install**. Tick *Erase device* for a fresh start; leave it unticked to update a
   board and keep its paired sensor and pod.
4. When it finishes, close the installer and press **Connect to the board**. The page
   reads the G7's name and 4-digit **pairing code**, the pod type, and the board's Wi‑Fi
   name and password straight from the board. (`status` on the console shows the same.)

### From source

1. Install [PlatformIO](https://platformio.org) (`brew install platformio` on a Mac) and
   get the code:

   ```bash
   git clone https://github.com/loudestnoise/ESP-Sim
   cd ESP-Sim
   ```

2. Plug in the board, then build and flash. The first build downloads the toolchain and
   takes a few minutes.

   ```bash
   pio run -t upload
   ```

3. Open the console to see the pairing code. Type `help` for commands; Ctrl+C quits.

   ```bash
   pio device monitor
   ```

### Updating, and starting over

- **Updating** keeps everything the board has stored: the sensor, the pod, their
  pairings and the Wi‑Fi network. Flash again from the browser with *Erase device*
  unticked, or run `git pull` and `pio run -t upload`.
- **Starting over** wipes all of that. Tick *Erase device* in the browser, or run
  `pio run -t erase` and then `pio run -t upload`. To replace only the sensor or only the
  pod, use the `new` and `pod fresh` console commands instead.

### If flashing fails

- **No port listed:** try another cable or USB port.
- **Stuck on "Preparing installation", or it won't connect:** the board hasn't entered
  download mode. Hold the board's **BOOT** button, tap **RESET**, release BOOT, then
  reload the page and try again. After installing, press RESET once to start the firmware.
- **"Port is busy":** close anything else using the port, such as another serial monitor
  or a second browser tab with the flasher open.
- **More than one board plugged in:** `pio device list` shows the ports; choose one with
  `pio run -t upload --upload-port /dev/cu.usbmodemXXXX`.
- **Nothing happens after flashing:** press RESET, or unplug the board and plug it back in.

### Hosting the flasher, or flashing by hand

`tools/make_flasher.sh` builds the browser flasher into `flasher/dist` (page, manifest,
the firmware parts, and a pinned copy of ESP Web Tools). Put that folder on any HTTPS
server; Web Serial does not work over plain HTTP.

The firmware is four parts at fixed addresses. With `esptool.py`:

```bash
esptool.py --chip esp32s3 write_flash \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Write them separately. A single merged image pads the gaps between the parts, and that
padding overwrites the board's stored pod state and Wi‑Fi settings.

Tested on an ESP32-S3 with 8 MB flash, with Trio on a test iPhone: a DASH pod and an
Omnipod 5 pod each paired, primed and ran, with the G7 paired on the same connection and
its readings collected every 5 minutes.

## Pair

- **G7:** add a Dexcom G7 in the app and enter the 4-digit pairing code the console prints.
- **Pod:** pair a new Omnipod DASH pod. For Omnipod 5, switch the pod type first (pod
  panel, or `pod mode o5`).

Use a **test phone**, never one that runs a real sensor or pod.

## Control

**Over USB, with buttons (no Wi‑Fi needed).** Open **https://loudestnoi.se/esp-sim/** in
Chrome or Edge, press **Connect to the board** and pick its port. The page shows the
pairing code, the latest reading and the pod, and has buttons for test situations: steady,
low, high, wave and random-walk glucose; sensor states such as warmup, a temporary sensor
issue or a failed sensor; pod reservoir, alerts, age and an occlusion fault; and starting
a new sensor or pod. Below the buttons is the board's log and a box for typing any
[command](#commands). Close other serial monitors first: only one program can use the
port.

**Web panels.** With no Wi‑Fi network saved, the board runs its own access point,
`ESP-Sim-XXXX`; its password is printed on the console at boot. Join it and open
`http://192.168.4.1/`. To put the board on your network instead, use the pod panel's Wi‑Fi
card or `wifi <name> <password>`; it then prints its address (also `http://esp-sim.local/`)
and takes the time from NTP.

- `/` is the G7 panel: glucose (fixed, sine, random walk), sensor errors, new sensor.
- `/pod` is the pod panel: reservoir, pod age, alerts, faults, DASH or Omnipod 5, new pod.

The JSON APIs are the originals': g7sim's at `/status`, `/glucose`, `/forget`, `/new`, and
ESP-Sim's under `/pod` (`/pod/status`, `/pod/command`, `/pod/wifi`).

The console commands below do everything the panels do, and more.

## Commands

There are two kinds: commands you type **into the board's console** to control the
simulators, and commands you run **on your computer** to build and flash.

### Opening the board's console

The board must be plugged in by USB. Only one program can use its port at a time, so
close one console before opening another.

**In a browser (Logs & Console).** Nothing to install; needs Chrome or Edge on a computer.

1. Open **https://loudestnoi.se/esp-sim/** and press **Connect and install**.
2. Pick the board's port, usually *USB JTAG/serial debug unit*.
3. Choose **Logs & Console**. (Don't choose Install; that reflashes the board.)
4. Type a command in the box at the bottom and press Enter. The reply appears in the log.
5. Close the window when you're done, so the port is free again.

**In a terminal.** Needs [PlatformIO](https://platformio.org) and a copy of this repo.

```bash
cd ESP-Sim
pio device monitor
```

Type a command and press Enter. What you type may not be shown until you press Enter.
Press **Ctrl+C** to quit.

Either way, start with `help` to list the commands, or `status` to see the pairing code.

### Console commands: G7 sensor

The sensor's commands are [LoopKit/g7sim](https://github.com/LoopKit/g7sim)'s control
API: the same four requests, with the same JSON fields. On the console, type the request's
name followed by the body you would post to g7sim.

| Console | g7sim | What it does |
|---|---|---|
| `status` | `GET /status` | Sensor name, pairing code, latest reading, plus the pod and Wi‑Fi details |
| `glucose {"kind":"fixed","value":65}` | `POST /glucose` | Hold glucose at one value (mg/dL) |
| `glucose {"kind":"sine","value":120,"amplitude":50,"periodMinutes":240}` | `POST /glucose` | Swing around 120 by ±50 over 240 minutes |
| `glucose {"kind":"walk","value":140}` | `POST /glucose` | Drift randomly toward 140 |
| `glucose {"kind":"fixed","value":120,"algorithmState":18}` | `POST /glucose` | Report a sensor error: 18 is a temporary issue, 25 a failed sensor |
| `forget` | `POST /forget` | Unpair every phone from the sensor. It then advertises continuously until paired again. |
| `new {"model":"stelo","code":"1234","ageMinutes":0}` | `POST /new` | Replace the sensor with a new one. The board restarts. |

As in g7sim, `glucose` replaces the whole mode: a number you leave out is 0, and leaving
out `algorithmState` returns the sensor to normal. `new` takes `model` (`g7`, `oneplus` or
`stelo`), `days` (10 or 15), `code` (four digits), `serial`, `ageMinutes` (0 starts in
warmup; the default is 120), `lifetimeHours` and `warmupMinutes`; anything left out picks
g7sim's default. `new` alone makes a sensor with a random name and code.

The same requests work over Wi‑Fi exactly as g7sim documents them, for example
`curl -XPOST esp-sim.local/glucose -d '{"kind":"fixed","value":65}'`.

Shorter forms for typing, which g7sim does not have:

| Shorthand | What it does |
|---|---|
| `glucose fixed 55` | Hold glucose at one value, keeping the current algorithm state |
| `glucose sine 120 50 90` | Mean, swing and period in minutes, keeping the algorithm state |
| `glucose walk 120` | Random walk, keeping the algorithm state |
| `algo 18` | Change only the algorithm state. `algo 0` returns to normal. |
| `new code=1234 ageMinutes=0` | The fields of `new` as `key=value` |
| `help` | List all commands |

Glucose changes take effect at the next 5-minute reading. After `forget` or `new`, pair
the sensor in the app again.

Not carried over from g7sim: `POST /force-quit` and the `-transport`, `-socket`,
`-simulator`, `-relaunch`, `-suspend` and `-dylib` flags, which only apply to its iOS
Simulator transport, and `-max-connections`, since the board serves one phone at a time.
`/status` reports times as seconds of sensor age (`ageSeconds`, `lastSeenAgoSeconds`,
`lockedForSeconds`) where g7sim gives clock times, because the board has no clock without
Wi‑Fi.

### Console commands: Omnipod pod

The pod's commands are [loopkit/pod](https://github.com/loopkit/pod)'s, with the same
names, values and limits as its control API. `pod <command> <value>` on the console is
`{"command": "<command>", "value": <value>}` posted to `/pod/command`.

| Command | What it does |
|---|---|
| `pod changeReservoir 45` | Set the reservoir, 0 to 200 U |
| `pod setActiveTime 4200` | Set how long the pod has been active, in minutes (0 to 6000) |
| `pod setAlerts 16` | Raise alert slots (a bit mask, 0 to 255). `pod setAlerts 0` clears them. |
| `pod setFault 20` | Raise a fault with that code. `pod setFault 0` clears it. |
| `pod crashNextCommand before` | Restart the board before it processes the next command that changes delivery, leaving the phone without an answer. `after` restarts once it has processed it. (`"beforeProcessing": true` or `false` in the API.) |
| `pod ignoreNextMessages 3` | Keep the link up but leave that many messages unanswered (0 to 100), as a pod that didn't hear them would |
| `pod failNextConnects 2 7` | Accepted and refused, as loopkit/pod refuses it over real Bluetooth: it needs loopkit/pod's ImpossiBLE transport, because a real radio cannot leave service discovery unanswered |

loopkit/pod takes its remaining choices as flags when it starts. A board has no command
line, so they are console commands here:

| Command | loopkit/pod | What it does |
|---|---|---|
| `pod` | `GET /status` | The pod's status |
| `pod fresh` | `-fresh` | Forget the pod and become a new, unpaired one of the same type |
| `pod mode dash` | `-mode dash -fresh` | Become a new, unpaired DASH pod |
| `pod mode o5` | `-mode o5 -fresh` | Become a new, unpaired Omnipod 5 pod |
| `pod selftest` | none | Check the pod's cryptography on this board |

`pod fresh` and `pod mode` end the pod the app is using: discard it in the app, then pair
the new one. The names this console used before (`pod new`, `type`, `reservoir`, `age`,
`fault`, `alerts`) still work.

### Console commands: the board

| Command | What it does |
|---|---|
| `wifi MyNetwork mypassword` | Join a Wi‑Fi network. The board restarts and prints its address. |
| `wifi off` | Forget the network and go back to the board's own access point |
| `scan` | List the Wi‑Fi networks the board can see (2.4 GHz only) |
| `reboot` | Restart the board. Sensor, pod and pairings are kept. |

### On your computer: build and flash

Run these in a terminal, from the repo's folder. They need PlatformIO.

| Command | What it does |
|---|---|
| `pio run` | Build the firmware without flashing |
| `pio run -t upload` | Build and flash. Keeps the board's sensor, pod, pairings and Wi‑Fi. |
| `pio run -t erase` | Wipe the board completely. Follow with `pio run -t upload`. |
| `pio device monitor` | Open the board's console |
| `pio device list` | List connected boards and their ports |
| `pio run -t upload --upload-port /dev/cu.usbmodemXXXX` | Flash one board when several are plugged in |
| `tools/make_flasher.sh` | Build the browser flasher into `flasher/dist` |
| `tools/make_flasher.sh user@host:folder` | Build it and upload it with rsync |

`make_flasher.sh` stops if there are uncommitted changes, because the flasher names the
commit it was built from.

### On your computer: test clients

Two Python scripts play the phone's side over Bluetooth from a Mac. Set them up once:

```bash
python3 -m venv venv
venv/bin/pip install bleak cryptography
```

| Command | What it does |
|---|---|
| `venv/bin/python tools/podcheck.py` | Find the pod and check that it answers |
| `G7NAME=DXCMab venv/bin/python tools/g7client.py 1234` | Pair with the G7 as a phone would. Use your board's name and code. |

`g7client.py` takes the sensor's phone slot, so the app has to pair again afterwards. Both
need the board free: one phone or computer can be connected at a time.

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
| `lib/dashpod/` | ESP-Sim's pod core with two commands added (GPL-3.0-or-later) |
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
