#pragma once
// The Bluetooth side: one peripheral that is both a G7 sensor and a pod.
//
// A phone cannot hold two connections to one radio, so the board cannot be
// two Bluetooth devices to the same phone. It is one device, at the chip's
// own address, whose GATT table carries both the G7's services and the pod's.
// The app's G7 code and its pod code each connect to "their" device and share
// the one link underneath.
//
// - Advertising: one set. Its payload is the pod's advertisement, the G7's,
//   or the two taking turns, depending on who needs to be found (the pod's
//   must carry exactly its own service list, so they cannot be merged).
// - The link: a real G7 hangs up after every reading, and the app's G7 code
//   only asks for a reading after connecting. So once per reading, when the
//   pod is quiet, the board drops the link; both sides reconnect at once and
//   the G7 code collects the new reading.

#include "dash.h"
#include "medtrum.h"
#include "sensor.h"

namespace radio {
void begin(Sensor* sensor);
// Call every ~50 ms: follows the sensor's advertising schedule and keeps the
// pod advertising.
void tick();
int openLinks();

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
}  // namespace radio
