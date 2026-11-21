#pragma once
// The Bluetooth side: one peripheral that is both a CGM (Dexcom G6 or G7) and
// a pod.
//
// A phone cannot hold two connections to one radio, so the board cannot be
// two Bluetooth devices to the same phone. It is one device, at the chip's
// own address, whose GATT table carries both the CGM's services and the pod's.
// The app's CGM code and its pod code each connect to "their" device and share
// the one link underneath.
//
// - Advertising: one set. Its payload is the pod's advertisement, the CGM's,
//   or the two taking turns, depending on who needs to be found (the pod's
//   must carry exactly its own service list, so they cannot be merged).
// - The link: a real CGM ends the session when the phone has its reading, and
//   the app's code only asks for a reading after connecting. So the core ends
//   the link by itself (cgmDisconnect, done from tick()), and both sides
//   reconnect at once.

// cgm.h is the verbatim portable core and has no extern "C" guard (upstream
// includes it from C only); this header is included from C++, so the guard goes
// here and not in the ported file.
extern "C" {
#include "cgm.h"
}
#include "dash.h"
#include "medtrum.h"

namespace radio {
// Sets up the GATT table and the advertising set. Call once the core has
// started (sensor::begin()) and before the pod's and patch's tasks.
void begin();
// Call every ~50 ms: applies a hang-up the CGM asked for, and keeps the one
// advertising set rotating between the CGM, the pod and the Medtrum patch.
void tick();
int openLinks();

// The CGM core's radio (the radio half of cgm_io_t, and the glue's own view of
// the link). cgmDisconnect only marks the hang-up: the core calls it from
// inside its own mutex, so the link is terminated from tick() instead.
void cgmNotify(cgm_chr_t chr, const uint8_t* p, size_t n);
void cgmDisconnect();
void cgmAdvertise(bool on, const char* name, const uint8_t addr[6]);
bool cgmConnected();

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
// phone cannot hold the link), PUMP_POD or PUMP_MEDTRUM (only that one). A phone
// runs one pump app, so the pod and the Medtrum base are mutually exclusive:
// there is no configuration in which both advertise. The CGM is a separate
// pane and shares the one link.
enum { PUMP_OFF = 0, PUMP_POD = 1, PUMP_MEDTRUM = 2 };
void setPump(int pump);
int pump();
}  // namespace radio
