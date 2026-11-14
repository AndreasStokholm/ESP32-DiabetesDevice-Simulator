#pragma once
// One central's connection and the sensor's side of it (port of g7/link.go and
// g7/handler.go). The BLE host task delivers the central's writes into the
// link's queue; a FreeRTOS task per link runs the protocol straight-line, the
// way the Go handler's goroutine does.

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <memory>

#include "sensor.h"

enum class Char { Auth = 0, Control, Backfill, Certificate };
const char* charName(Char c);

// The ATT value handles of the four CGM characteristics, set by the radio.
extern uint16_t charHandles[4];

struct InMsg {
  Char ch;
  bool subscribed = false;
  Bytes data;
};

class Link {
 public:
  Link(uint16_t conn, const std::string& addr, AdvertMode mode);
  ~Link();

  const uint16_t conn;
  const std::string addr;
  const AdvertMode mode;  // what the sensor was advertising when the link came up

  std::atomic<bool> connected{true};
  std::atomic<bool> finished{false};
  std::atomic<bool> served{false};  // the phone has collected a reading on this link
  std::atomic<uint16_t> subs[4];  // CCCD value per characteristic

  // From the BLE host task: a write or a subscription, in arrival order.
  void enqueue(Char c, const uint8_t* data, size_t len, bool subscribed);
  // Blocks up to timeoutMs for the next message; false on timeout or once the
  // link is gone.
  bool next(InMsg& out, int64_t timeoutMs);

  bool notify(Char c, const Bytes& b);
  void close();

 private:
  QueueHandle_t q;
};

// Starts the protocol task for a newly accepted link.
void startHandler(std::shared_ptr<Link> link, Sensor* sensor);
