# ESP32 Diabetes Device Simulator

One ESP32-S3 that simulates **one CGM and one pump at a time**, each chosen on its own page.
The CGM page (`/g7`) picks Off, a **Dexcom G6** transmitter or a **Dexcom G7** sensor; the
pump page (`/pump`) picks Off, an **Omnipod DASH** pod, an **Omnipod 5** pod or a **Medtrum
patch**. Everything not chosen is off: it does not advertise and does not hold one of the
board's two Bluetooth links. The CGM can serve **two centrals at once** (a phone and a
watch), as a real transmitter serves several receivers; the pump stays on one link. On a
test phone, add the chosen CGM in the app and the chosen pump (from Loop or Trio, or a
MedtrumKit-capable app): the app has a CGM and a pump, with no real hardware. A new board
starts as a Dexcom G7 and an Omnipod 5.

> **Test equipment only.** Not a Dexcom, Insulet or Medtrum product, and not a medical
> device. It delivers no insulin and every value it reports is made up. Never use it, or an
> app paired to it, for anyone's therapy.

It builds on two projects:

- the pod, the CGM and the Medtrum patch are [nightscout/ESP-Sim](https://github.com/nightscout/ESP-Sim)'s
  **portable C cores**, unmodified: `lib/dashpod` (commit `26c5219`, plus loopkit/pod's
  `ignoreNextMessages` and `failNextConnects` commands), `lib/cgmsim` (the G6/G7 protocol)
  and `lib/medtrumpatch`, each with small Arduino/PlatformIO glue;
- the CGM is the same code ESP-Sim runs, so there is one CGM engine on the board, not a
  port of [LoopKit/g7sim](https://github.com/LoopKit/g7sim); g7sim's ideas live on in the
  shape of this board's console and JSON API (see
  [Differences](#differences-from-the-originals)).

## How one board is a CGM and a pump

The board is **one peripheral** (one GATT table) that advertises as **two devices** at
once: the selected CGM on the sensor's address, and the selected pump on the board's. The
app's CGM code and its pump code each connect to "their" device. The board holds up to
**two Bluetooth links** at a time. The CGM may be held by **two centrals at once** (a phone
and a watch), which is what a real transmitter does; the selected pump (or the Medtrum
base) stays on one link, because it is one phone's pump. A third connection is refused and
logged.

- **Two advertising identities, using the addresses a real CGM and pump use.** The CGM
  peripheral advertises on the sensor's own **random static address**; the pump peripheral
  advertises on the board's device address. That is what a real sensor and a real pump look
  like, and it keeps the two advertisements apart: one address carrying both payloads
  garbles the service and manufacturer fields for the apps. The pump identity carries the
  selected pump's advertisement, and the CGM identity carries the CGM's (the `0xFEBC`
  service plus its name, `Dexcomxx` for a G6 or `DXCMxx` for a G7). Both advertise at the
  same time, so the phone sees the CGM and the pump as two devices simultaneously. The
  pod's (` :: Fake POD ::`) and the Medtrum base's (`MT` plus its serial) are **mutually
  exclusive** on the pump identity: exactly one of the two may use it, and the pump page's
  Off lets neither. Each keeps its own bytes exactly; they cannot be merged into one
  payload.
- **The CGM advertises around each reading**, as a real sensor does: a 60-second window
  after a reading while no central has collected it. While one CGM central is connected the
  window stays open as long as the second link is free, so a watch that scans after the
  phone connected can still find the sensor; it closes once both links are the CGM's, when
  the core's window ends, or when the hang-up starts.
- **The board hangs up when the reading is out**, once **every** CGM central subscribed to
  it has collected (a phone and a watch can take it at their own pace), never while the
  pump is talking, and never later than 20 seconds. The app's CGM code only asks for a
  reading after connecting, so this is what makes it collect the next one; the centrals
  reconnect within a second or two. The CGM core still models **one session**: the first
  central opens it and the last one leaving closes it, so a watch hanging up does not reset
  the phone's session.
- **Two centrals at once, a third refused.** The CGM may use both links (a phone and a
  watch); a pump phone and its CGM use one each. With two CGM centrals connected, a pump
  cannot also connect.

## Flashing the firmware

You need an **ESP32-S3 board with at least 8 MB of flash** and a USB **data** cable (some
cables only charge). If the board has two USB sockets, use the one marked *USB*, not
*UART* or *COM*.

It has to be an ESP32-S3. Look-alike boards with another chip, such as the Seeed XIAO
nRF52840, show up as a port but cannot be flashed: the installer waits on "Preparing
installation" forever. Other ESP32 kinds (the original ESP32, S2, C3, C6) are refused or
won't run this build.

### From a browser (easiest)

Nothing to install. Needs Chrome or Edge on a computer; Safari, Firefox and phones can't
flash.

1. Open **https://loudestnoi.se/esp-sim/** and plug in the board.
2. Press **Connect and install** and pick the board's port, usually listed as
   *USB JTAG/serial debug unit*.
3. Choose **Install**. Tick *Erase device* for a fresh start; leave it unticked to update a
   board and keep its paired sensor and pod.
4. When it finishes, close the installer and press **Connect to the board**. The page
   reads the CGM's **name**, the pod type, and the board's Wi‑Fi
   name and password straight from the board. (`status` on the console shows the same.)
5. Pair from the app (see [Pair](#pair)), and use the same page's buttons to
   [control](#control) the board.

### From source

1. Install [PlatformIO](https://platformio.org) (`brew install platformio` on a Mac) and
   get the code:

   ```bash
   git clone https://github.com/loudestnoise/ESP32-DiabetesDevice-Simulator
   cd ESP32-DiabetesDevice-Simulator
   ```

2. Plug in the board, then build and flash. The first build downloads the toolchain and
   takes a few minutes.

   ```bash
   pio run -t upload
   ```

3. Open the console and type `status` to see the CGM's kind, name and id. Type `help` for commands; Ctrl+C quits.

   ```bash
   pio device monitor
   ```

### Updating, and starting over

- **Updating** keeps everything the board has stored: the CGM, the pod, the patch, the
  pod's and patch's pairings, and the Wi‑Fi network. Flash again from the browser with
  *Erase device* unticked, or run `git pull` and `pio run -t upload`.
- **Starting over** wipes all of that. Tick *Erase device* in the browser, or run
  `pio run -t erase` and then `pio run -t upload`. To replace only the sensor, pod or
  patch, use `new`, `pod fresh`, or **New patch** on the pump page's Medtrum panel
  instead.

### If flashing fails

- **No port listed:** try another cable or USB port.
- **Stuck on "Preparing installation", or it won't connect:** the board hasn't entered
  download mode. Hold the board's **BOOT** button, tap **RESET**, release BOOT, then
  reload the page and try again. After installing, press RESET once to start the firmware.
  If that never helps, check the board really is an ESP32-S3 (see above).
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
Omnipod 5 pod each paired, primed and ran, with a G7 sensor paired on the CGM link and its
readings collected every 5 minutes. Switching the pod type from the USB panel and
pairing the new pod was tested both ways. That test ran the g7sim-era sensor; the G6/G7
core (and the G6 mode in particular) has not been on hardware yet.

## Pair

- **CGM:** add a Dexcom G7 in the app; it finds the board as `DXCMxx` (`xx` is the id the
  panel shows), and there is **no pairing code**: the board reports itself as already
  bonded. For a **G6**, pick **Dexcom G6** in the CGM page's **Simulate** box, then enter the
  **6-character transmitter id** there (`81ABC2`-style: digits and capitals, no I or O) and
  the same id in the app; the board then advertises as `Dexcomxx` with that transmitter's
  id.
- **Pump:** choose the pump in the pump page's **Simulate** box. Pick **Omnipod 5** (what a
  new board starts as) or **Omnipod DASH** and pair a new pod of that type; switching type
  starts a new, unpaired pod. Pick **Medtrum patch** and activate a patch in an app with
  MedtrumKit support: the base serial is `4A12D828` for a 200 U base (MD0201) and
  `52DF1614` for a 300 U base (MD8301) -- the panel shows the same string, refuses a serial
  the app would reject, and advertises it reversed (`28 D8 12 4A`), because the app matches
  the advertisement against the reverse of what you type. The patch panel's **Set model** swaps the serial and
  model together; switching either ends the current activation, so the app activates a
  fresh patch.

Use a **test phone**, never one that runs a real sensor, pod or patch.

## Control

**Over USB, with buttons (no Wi‑Fi needed).** Open **https://loudestnoi.se/esp-sim/** in
Chrome or Edge, press **Connect to the board** and pick its port. Close other serial
monitors first: only one program can use the port. The page reads the board's console and
sends console commands, so a board whose firmware is older than the page may not know them.
The page's own sensor buttons were written for the g7sim-era firmware: this build has no
pairing code, and its glucose controls are the next reading plus a trend (see
[the CGM commands](#console-commands-the-cgm)). The page's console box runs any
[command](#commands); the board's own web panels (below) are the current UI.

**Web panels (over Wi‑Fi).** With no Wi‑Fi network saved, the board runs its own access
point, `ESP-Sim-XXXX`. Its name and password are shown by the USB panel and by `status`,
and printed on the console at boot. The password is derived from the chip's MAC (four MAC
bytes mixed with constants and prefixed `pod-`), so no credential is built into the
firmware.

Join the AP from a phone and it gets the Wi‑Fi sheet without typing an address. A
`DNSServer` answers every name with the board's AP address, the OS captive-portal probes
are answered the way Apple, Android and Windows expect, and `/` redirects to `/setup`
until the sheet has been dismissed. Once a network is saved or the sheet is dismissed, the
same probes return their "no portal" answers so the phone stops presenting it. The sheet
itself never shows the AP password: the reader is already on the AP.

- **Join and restart** saves a 2.4 GHz network and restarts the board on it; the pages
  then stay at `http://esp-sim.local/` and the clock is set from NTP. If the network cannot
  be joined, the board comes back as its own access point and the sheet returns.
- **Continue without joining** sets a `setup=done` cookie (path `/`, 7 days), goes to `/`,
  and leaves the simulators running offline. Only the board's wall clock is unset; the CGM
  keeps its own clock, and the pod and patch start from their own uptime.

The sheet is also at `http://192.168.4.1/setup`. To put the board on your network later,
use `/settings` or `wifi <name> <password>`.

All pages share one nav: **Home, CGM, Pump, Settings**.

- `/` is the home page: two cards, **CGM** and **Pump**, each showing the selected device
  and its live line (the CGM's kind, name, glucose and state from `/g7/status`; the selected
  pump's type, pairing, reservoir and connection), plus a no-clock banner that links to
  `/setup`.
- `/g7` is the CGM page: a **Simulate** box at the top (Off / Dexcom G6 / Dexcom G7), then
  a G6 transmitter id, the next reading and its trend, a calibration, the sensor state,
  signal loss, skip warm-up, a new sensor, and the failure-injection card.
- `/pump` is the pump page: a **Simulate** box at the top (Off / Omnipod DASH / Omnipod 5 /
  Medtrum patch), then the selected pump's panel only — the pod's reservoir, pod age,
  alerts, faults, crash and new pod, or the patch's model and serial, reservoir, state,
  fault, crash and new patch. `/pod` and `/medtrum` are aliases for this page, so an old
  bookmark still works.
- `/settings` owns the board: joined SSID and address, clock/NTP state, join or forget the
  network, the selected pump (a read-out that links to `/pump`), firmware, hardware, and
  reboot. Wi‑Fi setup left the device pages for here.
- `/setup` is the captive-portal Wi‑Fi sheet (also at `http://192.168.4.1/setup`).

The pod and the Medtrum base are **mutually exclusive**: a phone runs one pump app, and the
pump uses one BLE link, so only one pump can hold it. They share the pump advertising
identity: selecting a pump stops the other one advertising and drops any link it held;
selecting Off leaves no pump advertising and no pump connection accepted. The CGM's
advertising identity and its own links are unaffected either way.

The JSON APIs are the originals': g7sim's four stay at the root (`/status`, `/glucose`,
`/forget`, `/new`), and the CGM's own core has `/g7/status` and `/g7/command`; loopkit/pod's
are under `/pod` (`/pod/status`, `/pod/command`), the patch's under `/medtrum`
(`/medtrum/status`, `/medtrum/command`), plus `/settings/status`, `/settings/pump` (the
selector), `/settings/wifi`, `/settings/forget`, `/settings/reboot` and `/fail` (see
[Failure injection](#failure-injection)). `/setup/wifi`, `/pod/wifi` and `/medtrum/wifi`
are aliases to the same save handler.

The console commands below cover the CGM, the pod and the board; the Medtrum patch is
controlled from the pump page or `/medtrum/command`.

## Commands

There are two kinds: commands you type **into the board's console** to control the
simulators, and commands you run **on your computer** to build and flash.

### Opening the board's console

The board must be plugged in by USB. Only one program can use its port at a time, so
close one console before opening another.

**In a browser.** Nothing to install; needs Chrome or Edge on a computer.

1. Open **https://loudestnoi.se/esp-sim/** and press **Connect to the board**.
2. Pick the board's port, usually *USB JTAG/serial debug unit*.
3. Scroll to **Console**, type a command in the box and press Enter. The reply appears in
   the log above it.
4. Press **Disconnect** when you're done, so the port is free again.

The installer's own **Logs & Console** window (under **Connect and install**) works too.
Don't choose Install there unless you mean to reflash the board.

**In a terminal.** Needs [PlatformIO](https://platformio.org) and a copy of this repo.

```bash
cd ESP32-DiabetesDevice-Simulator
pio device monitor
```

Type a command and press Enter. What you type may not be shown until you press Enter.
Press **Ctrl+C** to quit.

Either way, start with `help` to list the commands, or `status` to see the CGM's kind,
name and id with the pod and Wi‑Fi details.

### Console commands: the CGM

The CGM is ESP-Sim's portable G6/G7 core. The console keeps [LoopKit/g7sim](https://github.com/LoopKit/g7sim)'s
four requests at the root and the same JSON fields, so the old one-liners still work, but
what they do is what the core can do: one next reading and a trend, not g7sim's waveforms.

| Console | g7sim | What it does |
|---|---|---|
| `status` | `GET /status` | CGM kind, name, id, the next reading, the last reading, plus the pod and Wi‑Fi details |
| `glucose {"kind":"fixed","value":65}` | `POST /glucose` | The next reading is 65 mg/dL |
| `glucose {"kind":"fixed","value":120,"trend":-10}` | `POST /glucose` | 120 mg/dL now, and the next readings fall (the trend is tenths of mg/dL per minute, so −10 is about −5 mg/dL every 5 minutes) |
| `glucose {"kind":"fixed","value":120,"algorithmState":18}` | `POST /glucose` | Report a sensor error: 18 is a temporary issue, 16 a G6 session failure, 25 a failed sensor |
| `cal 120` | (none) | A G6 calibration: the last calibration and the next reading, as a 0x34 write does |
| `algo 18` | (shorthand) | Change only the reported state; `algo 0` lets the core derive it |
| `kind g6` / `kind g7` / `kind off` | (none) | Become a G6 transmitter, a G7 sensor, or neither. A new kind starts a new sensor |
| `id 81ABC2` | (none) | A G6's 6-character transmitter id (2 characters for a G7) |
| `forget` | `POST /forget` | Drop all CGM links. The core keeps no pairing to forget |
| `new {"model":"g7","ageMinutes":0}` | `POST /new` | Start a new sensor. The board restarts |

The same requests work over Wi‑Fi, for example
`curl -XPOST esp-sim.local/glucose -d '{"kind":"fixed","value":65}'`.

`glucose` keeps g7sim's body: a number you leave out is 0, and leaving out
`algorithmState` returns the sensor to normal. g7sim's `sine` and `walk` are accepted and
logged as not applicable: the core has no waveform, so the `value` is the next reading and
`trend` (tenths of mg/dL per minute, applied over the five minutes to the following
reading) is how it moves. `cal`,
`kind` and `id` are additions for the G6; the panel's signal loss and skip warm-up are
`/g7/command` only (below).

`new` takes g7sim's body too: `model` picks the kind (`g6` is an extension; `g7`, `oneplus`
and `stelo` are all a G7 sensor here), `ageMinutes` starts an older sensor (0 starts in
warm-up), and `days`, `code`, `serial`, `lifetimeHours` and `warmupMinutes` are accepted,
logged as not applicable and not invented. The core's sessions are fixed: a G6 is a 10-day
transmitter with a 2-hour warm-up, a G7 a 10.5-day sensor (10 days and the 12-hour grace)
with a 27-minute warm-up. There is no pairing code to choose.

The panel and `/g7/command` take the core's own commands:

| `/g7/command` | What it does |
|---|---|
| `{"command":"cgmSetGlucose","glucose":120,"trend":-10}` | The next reading and its trend (tenths of mg/dL per minute) |
| `{"command":"cgmSetState","state":18}` | Force the reported state (0 derives it from the session's age) |
| `{"command":"cgmSetSignalLoss","on":true}` | Signal loss: stop advertising and drop the CGM's links; the readings are still made, so the app backfills them when it ends |
| `{"command":"cgmSkipWarmup"}` | End the warm-up now, keeping the sensor and its id |
| `{"command":"cgmSetCalibration","glucose":120}` | G6 calibration (the last calibration and the next reading) |
| `{"command":"cgmSetKind","kind":6}` | 0 off, 6 G6, 7 G7; a new kind starts a new sensor |
| `{"command":"cgmSetId","id":"81ABC2"}` | A G6's transmitter id (2 characters for a G7) |
| `{"command":"cgmNewSensor","kind":7,"ageMinutes":120}` | A new sensor of that kind, that many minutes old |
| `{"command":"cgmForget"}` | Drop all CGM links |

`GET /g7/status` returns the core's own state (kind, name, id, glucose, trend, state,
`signalLoss`, the reading history's last entry, `advertising`/`connected`, plus
`radioConnected`, `now`, `ageSeconds` and `warmupLeftSeconds`), and `centrals` /
`collectedCentrals` make a two-central test readable. The panel polls it every 2 seconds.

Glucose changes take effect at the next 5-minute reading. After `new`, `kind` or `id`, the
app has to find the sensor again (a new sensor has a new id).

What g7sim had and this core does not: the 4-digit pairing code and the J-PAKE handshake
(the core auto-authenticates and reports itself bonded), per-display key slots (see
[Differences](#differences-from-the-originals)), the ONE+/Stelo advertised names, and the
sine/random-walk waveforms. `GET /status` keeps g7sim's field names, but `code` is empty,
`slots` is `{}` and `glucoseMode.kind` is always `"fixed"`, because those are what the core
has; g7sim's `window`, `activation` and `lockedForSeconds` are not reported either (the
core has no per-reading window view, no wall-clock activation and no rejection lockout).
Not carried over either: `POST /force-quit` and the `-transport`, `-socket`,
`-simulator`, `-relaunch`, `-suspend` and `-dylib` flags, which only apply to g7sim's iOS
Simulator transport, and `-max-connections`, since the board has its own two-link limit.
`/status` reports times as seconds of sensor age (`ageSeconds`) where g7sim gives clock
times, because the board has no clock without Wi‑Fi (the CGM keeps its own clock anyway).

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
| `reboot` | Restart the board. The CGM's, pod's and patch's state is kept. |

### On your computer: build and flash

Run these in a terminal, from the repo's folder. They need PlatformIO.

| Command | What it does |
|---|---|
| `pio run` | Build the firmware without flashing |
| `pio run -t upload` | Build and flash. Keeps the board's CGM, pod, patch and Wi‑Fi. |
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
| ~~`G7NAME=DXCMab venv/bin/python tools/g7client.py 1234`~~ | **Retired**: it speaks g7sim's J-PAKE pairing, which this firmware no longer has. Use the app instead. |

`g7client.py` also took the sensor's link (the old firmware had only one), so the app had
to pair again afterwards. Both need a link the app is not using: the board has two, and the
app may already hold one or both.

## Tests

The host suites need only `cc` and `make` (no PlatformIO, no board):

| Command | What it covers |
|---|---|
| `make -C test/cgm` | The CGM core, with ESP-Sim's own checks (the sources are byte-identical) |
| `make -C test/medtrum` | The Medtrum patch core |
| `make -C test/fail` | The failure-injection decisions (drop, refuse, quiet, delay, ignore) for each device |
| `make -C test/portal` | The captive-portal probe and redirect decision |

The items that need a board: the NimBLE wiring (`src/radio.cpp`), the CGM's NVS
blob and its g7sim-state migration (`src/sensor_app.cpp`), and the panels. They
depend on the Arduino platform (NimBLE, Preferences/NVS, LittleFS, ArduinoJson),
so they are exercised on a board; a host test for them would first need the
platform split behind a fake-able interface.

## Failure injection

The **Failure injection** card on `/g7` and on the selected pump's panel on `/pump`, the
console's `fail` command and `/fail` put real Bluetooth failures in front of the simulators. All five knobs
are enforced by the radio; the values live in RAM and reset on reboot.

| Knob | What it does | pod | CGM (`g7`) | Medtrum |
|---|---|---|---|---|
| `drop` | Accept the next connection, then disconnect at once | yes | yes | yes |
| `refuse` | Drop the next N connections | yes | yes | yes |
| `quiet` | Leave that device out of its advertising set for the chosen time | yes | yes | yes |
| `delay` | Wait before each outgoing notification from that device | yes | yes | yes |
| `ignore` | Drop the next N messages the phone sends to that device | yes | yes | yes |

The knobs reach the radio through `/fail`, and the three device cards all have them: drop,
refuse, quiet, delay, ignore, Apply and Clear. Post JSON to `/fail`:

```bash
curl -XPOST esp-sim.local/fail -H 'Content-Type: application/json' \
  -d '{"device":"pod","refuse":3,"quietMs":10000,"delayMs":500,"ignore":2}'
```

| Field | What it does | Range |
|---|---|---|
| `device` | Which simulator: `pod`, `g7` (also `cgm`), `medtrum` or `all` | |
| `drop` | Drop the next connection once for that device (accept it, then disconnect) | `true` / `false` |
| `refuse` | Drop this many subsequent connections | 0–1000 |
| `quietMs` | Stop advertising that device for this many milliseconds | 0–86,400,000 |
| `delayMs` | Wait this long before each outgoing notification | 0–60,000 |
| `ignore` | Drop this many messages from the phone, then resume | 0–1000 |

Omitted fields are left alone; `0` or `false` clears a field. The reply is the new state for
all three devices, e.g. `{"pod":{...},"g7":{...},"medtrum":{...}}` with `drop`, `refuse`,
`quietMs`, `delayMs` and `ignore` for each.

On the console:

```
fail g7 drop
fail pod refuse 3
fail medtrum quiet 10000
fail g7 delay 500
fail pod ignore 2
fail all clear
```

The board has two BLE links for all three simulators. `drop` and `refuse` are per device:
when only one device could be connecting (the other set is off) the decision is taken at
the connection; when both advertise it is taken at the connection's first GATT event, which
names the device, so a pending pod drop does not consume the CGM's connection. A refused
connection is accepted and then disconnected immediately. `device:"all"` with `drop:true`
arms one drop for each device. `quiet` only clears that device's advertising want flag: the
pod's and Medtrum's advertisement bytes do not change. `ignore` drops the message at the
BLE write, for all three devices.

The CGM's notifications are sent from a task of its own, so `delay` holds them without
blocking the CGM core. A delayed message is sent to every central that was subscribed when
it was queued: if one has hung up by then, that copy is dropped rather than delivered to a
new connection that reused the handle.

## Good to know

- All three simulators keep their state in flash: a reboot is the same CGM, pod and
  patch, still paired.
- The CGM keeps its own clock: a reboot continues the sensor's age, and NTP is not needed
  for it. Without NTP, the pod's and patch's clocks pause while the board is unplugged.
- In the app the pod may briefly show as reconnecting after each CGM reading. That's the
  hang-up described above.
- Unlike a real CGM, the board serves a phone that has already collected the current
  reading. It does serve a second display (a watch or another phone), up to two CGM
  centrals at once; a third is refused.
- The simulators don't appear in iOS Settings → Bluetooth. That's expected.
- Not yet tested on hardware: the captive Wi-Fi sheet on iOS and Android; long runs; what
  the app does after `crashNextCommand` and `ignoreNextMessages` (the board accepts them);
  and a blank board starting as an Omnipod 5 (boards updated from an earlier build keep the
  pod they had).
- **Not yet on hardware at all**: the G6/G7 core (`lib/cgmsim`) and the CGM's side of the
  radio. The panel sets the next glucose, a trend and the state; every reading is made up,
  and there is no waveform.
- The ESP32 is 2.4 GHz only, and can't sign in to captive-portal Wi‑Fi (hotels). Use its
  own access point or a phone hotspot.

## To-Do

Devices we want to add:

- [x] Omnipod DASH
- [x] Omnipod 5
- [x] Dexcom G6 (transmitter mode: a 6-character transmitter id)
- [x] Dexcom G7
- [x] Medtrum
- [ ] FreeStyle Libre 3 Plus (L3P)
- [ ] More to come!

## Differences from the originals

- **ESP-Sim** is an ESP-IDF project; here its **portable cores** run under Arduino/PlatformIO
  with new board glue in place of its `main/`: `lib/dashpod` (`src/pod.cpp`), `lib/cgmsim`
  (`src/sensor_app.cpp`) and `lib/medtrumpatch` (`src/medtrum_app.cpp`), plus each device's
  side of `src/radio.cpp`. The pod's panel moved to the pump page at `/pump`, where the pump
  type is one of the Simulate box's choices rather than a dropdown. A new board starts as an
  Omnipod 5, where ESP-Sim starts as a DASH. The pod core gained loopkit/pod's
  `ignoreNextMessages` and `failNextConnects` commands. ESP-Sim's host tests are not copied,
  except the CGM's (`make -C test/cgm`, the same checks ESP-Sim runs).
- **ESP-Sim's Medtrum patch** is ported here as a third device. Its portable patch core
  (`lib/medtrumpatch`, plain C with no ESP-IDF or mbedTLS) runs under Arduino/PlatformIO
  with new board glue (`src/medtrum_app.cpp`, and the patch's side of `src/radio.cpp`).
  The radio advertises as two devices at once: the CGM peripheral on the sensor's random
  static address, and the selected pump's on the board's device address (the pod's, or the
  Medtrum base's). The pod and the Medtrum base are mutually exclusive, so at most one of
  them is ever in the pump identity. One field is deliberately NOT ESP-Sim's:
  the base's manufacturer **company id is 0x4781 (18305)**, where ESP-Sim sends `0x0000`. A
  real base sends Medtrum's own id, an app that gates on it (Rockit) rejects a placeholder,
  and the point of the thing is to be the real device. Trio matches on the name `MT` alone.
  For the same reason the base's serial is advertised **reversed** relative to the string the
  panel shows: that is the form both apps compare against.
  The patch's panel is the pump page at `/pump` (the Medtrum patch choice), its host tests
  run with `make -C test/medtrum`, and its controls are the panel and `/medtrum/command`
  (there is no console command prefix for it).
- **The CGM is ESP-Sim's core**, not the g7sim port. `lib/cgmsim` is ESP-Sim's portable
  G6/G7 protocol, verbatim, with `src/sensor_app.cpp` for its clock, flash and task and the
  CGM's side of `src/radio.cpp`. The g7sim port that used to run the G7 (`src/sensor.cpp`,
  `handler.cpp`, `crypto.cpp`, `link.h`) is still in the tree but **retired**: nothing
  starts it. In exchange for g7sim's extras, the board gains a real **G6 transmitter mode**
  (a 6-character id, G6 calibration, a 2-hour warm-up and a 10-day session), signal loss
  with backfill, skip warm-up, and the full sensor-state table.
- **g7sim's interface is kept where it can be**: the panel is at `/g7`, and `/status`,
  `/glucose`, `/new` and `/forget` stay exactly where g7sim has them, with g7sim's field
  names and its JSON bodies. `/g7/status` and `/g7/command` are new (the panel uses them).
  What the core cannot do is not faked: `code` is empty, `slots` is `{}` and
  `glucoseMode.kind` is always `"fixed"`.
- **The honest costs of the swap**:
  - **Multi-display key slots are gone.** The core presents itself as already
    authenticated and bonded and keeps no per-display keys, so there is no slot table, no
    last-seen or miss count, and `/forget` drops all of the CGM's links. **Two centrals at a
    time** (a phone and a watch) are served, without per-display keys; a third is refused.
  - **No pairing code.** g7sim's 4-digit code and J-PAKE handshake are gone; a G7 is found
    as `DXCMxx` and a G6 needs its transmitter id, as the real apps expect.
  - **No sine or random-walk waveform.** The core holds **one next glucose and a trend**, so
    `/glucose`'s `value` is the next reading and `trend` is how it moves; `sine`/`walk` and
    their swing parameters are accepted and logged as not applied. The adapter does not
    synthesize them.
  - **`ONE+` and `Stelo` names are gone**: a G7 always advertises `DXCM`+id and a G6
    `Dexcom`+the id's last two characters, as ESP-Sim's reference does.
  - A board updated from the g7sim-era firmware migrates its saved sensor on first boot
    (the id suffix, age, glucose and up to 3 hours of readings); the pairing code, slots,
    calibration and waveform are dropped, each with a line on the console.
- **The retired g7sim port** (`src/sensor.cpp`, `handler.cpp`, `crypto.cpp` and their
  headers) is kept in the tree for reference but left out of the build by
  `platformio.ini`'s `build_src_filter`, so it costs no flash and cannot be compiled back
  in by accident.
- **Pages and the captive portal** are new here: `/` is a home page, `/g7` the CGM page,
  `/pump` the pump page (`/pod` and `/medtrum` alias it), and `/settings` the board
  (network, clock, firmware, reboot). The board's own access point answers as a captive
  portal and serves the Wi-Fi sheet at `/setup`; dismissing it sets a `setup=done` cookie.
  The failure-injection card is on the CGM page and on the selected pump's panel, backed by
  `/fail`.

## Layout

| Path | What |
|---|---|
| `lib/dashpod/` | ESP-Sim's pod core with two commands added (GPL-3.0-or-later) |
| `lib/cgmsim/` | ESP-Sim's portable Dexcom G6/G7 core, verbatim (GPL-3.0-or-later) |
| `lib/medtrumpatch/` | ESP-Sim's portable Medtrum patch core (GPL-3.0-or-later) |
| `src/pod.cpp` | The pod's clock, storage and task |
| `src/sensor_app.cpp` | The CGM's clock, storage and task |
| `src/medtrum_app.cpp` | The Medtrum pump base's clock, storage and task |
| `src/radio.cpp` | The CGM and pump advertising identities and their one GATT server/link |
| `src/sensor.cpp`, `handler.cpp`, `crypto.cpp`, `link.h` | The retired g7sim port (kept for reference, left out of the build) |
| `src/main.cpp` | Boot, Wi‑Fi, captive-portal routes, web pages, failure API, console |
| `src/portal.h`, `src/portal.cpp` | The pure captive-probe and AP-mode redirect decision |
| `src/fail.h`, `src/fail.cpp` | The pure failure-injection state machine |
| `src/home_html.h` | The home page with two cards, CGM and Pump |
| `src/index_html.h` | The CGM page |
| `src/pump_html.h` | The pump page (generated) |
| `src/pod_html.h`, `src/medtrum_html.h` | The pod and patch panels (generated; now folded into the pump page) |
| `src/settings_html.h` | The Settings page (Network, pump read-out, Board) |
| `src/setup_html.h` | The captive-portal Wi‑Fi sheet |
| `src/adv_payload.h` | The pure builders for the three advertising payloads and the name field |
| `test/cgm/` | Host tests for the CGM core |
| `test/medtrum/` | Host tests for the Medtrum patch core |
| `test/portal/` | Host tests for the captive-portal decision |
| `test/fail/` | Host tests for the failure-injection state |
| `flasher/`, `tools/make_flasher.sh` | The browser flasher |
| `tools/pump_panel.html`, `tools/gen_pump_panel.py` | The pump page's shell and its generator (reuses the pod and Medtrum panels) |
| `tools/pod_panel.html`, `tools/gen_pod_panel.py` | The pod panel source and its generator |
| `tools/medtrum_panel.html`, `tools/gen_medtrum_panel.py` | The patch panel source and its generator |
| `tools/g7client.py`, `tools/podcheck.py` | Test clients for a Mac (`g7client.py` speaks the retired g7sim pairing and no longer works) |

## License

GPL-3.0-or-later (see `LICENSE`), as ESP-Sim's pod, CGM and patch cores require of a
combined work. The retired g7sim port's files (now unused, `src/sensor.cpp`,
`handler.cpp`, `crypto.cpp`, `link.h`) come from MIT-licensed code (LoopKit/g7sim and
g7simESP32); their notice is in `LICENSE-g7sim-MIT`. The Medtrum patch core carries
MedtrumKit's CRC-8 and Rijndael tables (MIT, Copyright (c) 2024 J. Bunt); that notice is in
`lib/medtrumpatch/LICENSE.medtrumkit`.
