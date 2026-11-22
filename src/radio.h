#pragma once
// The Bluetooth side: one peripheral that is both a CGM (Dexcom G6 or G7) and
// a pod.
//
// A phone cannot hold two connections to one radio, so the board cannot be
// two Bluetooth devices to the same phone. It is one peripheral (one GATT
// table) that advertises under two addresses, as ESP-Sim does: the
// pump set uses the chip's own (device) address, the CGM set the sensor's
// random static address. The app's CGM code and its pod code each connect to
// "their" device and share the GATT table underneath.
//
// The CGM may be held by two centrals at once (a phone and a watch), as a real
// transmitter serves several receivers. The pump stays single-link: the pod
// and the Medtrum base are one phone's pump.
//
// - Advertising: two sets, as ESP-Sim numbers them. Instance 0 is the pump's:
//   the pod's advertisement, or the Medtrum base's, whichever the selector
//   chose (the pod and the base are mutually exclusive), on the device
//   address. Instance 1 is the CGM's (G6 or G7), whenever the core has a window
//   open, on the per-sensor random static address the core supplies. The pod's
//   and the base's advertisements each carry exactly their own service list, so
//   they cannot share a set; the two sets advertise at the same time.
// - The link: up to two concurrent links (kMaxLinks), one core CGM session per
//   link. A real CGM ends its session when a phone has its reading, and the
//   app's code only asks for a reading after connecting. The core ends each
//   session by itself (cgmDisconnect(session), done from tick()), and only that
//   central reconnects; another central's session is untouched.

// cgm.h is the verbatim portable core and has no extern "C" guard (upstream
// includes it from C only); this header is included from C++, so the guard goes
// here and not in the ported file.
extern "C" {
#include "cgm.h"
}
#include "dash.h"
#include "medtrum.h"

namespace radio {
// Sets up the GATT table and the advertising sets. Call once the core has
// started (sensor::begin()) and before the pod's and patch's tasks.
void begin();
// Call every ~50 ms: applies a hang-up the CGM asked for, and keeps the pump
// set (instance 0) and the CGM set (instance 1) advertising what they should.
void tick();
// How many centrals the radio currently holds (0..kMaxLinks: a CGM central
// and/or a pump phone).
int openLinks();

// The CGM core's radio (the radio half of cgm_io_t, and the glue's own view of
// the link). Each call names the core session; the radio maps that token to the
// link that central is on. cgmDisconnect only marks that session's hang-up: the
// core calls it from inside its own mutex, so the link is terminated from
// tick() instead.
void cgmNotify(cgm_session_t session, cgm_chr_t chr, const uint8_t* p, size_t n);
void cgmDisconnect(cgm_session_t session);
// serialCrc is CRC-16/XModem of the CGM id, computed by the glue under the
// core lock: computing it here would take that lock again and deadlock.
// addr is the sensor's random static address: the CGM set advertises from it;
// the pump set keeps the device address.
void cgmAdvertise(bool on, const char* name, const uint8_t addr[6], uint16_t serialCrc);
bool cgmConnected();  // true while any CGM central is on a link

// The pod's radio (the radio half of ESP-Sim's dash_io_t).
bool podNextEvent(dash_ev_t* ev, uint32_t timeoutMs);
void podNotifyCmd(const uint8_t* p, size_t n);
void podNotifyData(const uint8_t* p, size_t n);
void podDisconnect();
void podAdvertise(const dash_adv_t* adv);
bool podConnected();

// The Medtrum patch's radio (the radio half of ESP-Sim's medtrum_io_t).
bool medtrumNextEvent(medtrum_ev_t* ev, uint32_t timeoutMs);
void medtrumNotify(const uint8_t* p, size_t n);        // WRITE: responses
void medtrumNotifyStatus(const uint8_t* p, size_t n);  // READ: notifications
void medtrumDisconnect();
void medtrumAdvertise(bool on, const uint8_t sn[4]);
bool medtrumConnected();

// Which pump simulator is selected: PUMP_OFF (neither advertises, and a pump
// phone cannot hold a link), PUMP_POD or PUMP_MEDTRUM (only that one). A phone
// runs one pump app, so the pod and the Medtrum base are mutually exclusive:
// there is no configuration in which both advertise. The CGM is a separate
// pane and may hold the other link.
enum { PUMP_OFF = 0, PUMP_POD = 1, PUMP_MEDTRUM = 2 };
void setPump(int pump);
int pump();
}  // namespace radio
