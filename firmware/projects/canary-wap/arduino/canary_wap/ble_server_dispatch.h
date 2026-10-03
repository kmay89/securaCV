/*
 * SecuraCV Canary — one set of NimBLE server callbacks, two owners (sweep F171)
 *
 * NimBLE keeps one NimBLEServerCallbacks pointer per server
 * (NimBLEServer::setCallbacks), and the WAP has one server
 * (NimBLEDevice::createServer() is a singleton). Two modules want its
 * callbacks:
 *
 *   - the pairing channel (bluetooth_channel.cpp): every callback. A link up
 *     and down, the passkey it shows, the Numeric Comparison the owner must
 *     confirm in the web UI (require_pin), the bond, and the rest;
 *   - Opera (ble_opera.h, FULL profile): a link up and down, to count them
 *     and to re-advertise.
 *
 * Until F171 each called setCallbacks() with its own object, and on the
 * FULL profile Opera's init() runs after the channel's, so Opera's object
 * replaced the channel's. OperaServerCallbacks overrides only onConnect and
 * onDisconnect, so NimBLE-Arduino's defaults answered the rest: its default
 * onConfirmPassKey injects yes (NimBLEServer.cpp, 2.3.8 and 2.5.0), which
 * accepted every phone's pairing with no owner confirm and no MITM check,
 * and its default onPassKeyDisplay shows 123456. The channel never saw a
 * link, a passkey or a bond there.
 *
 * Now neither module calls setCallbacks(). Each hands its object to
 * install() with its role, and install() puts the one Dispatcher below on
 * the server, so the init order does not matter (the boot worker runs the
 * channel's first and Opera's second, but a REST handler's bring_up() can
 * run the channel's after Opera's). The Dispatcher hands:
 *
 *   - every callback to the pairing owner (kPairing);
 *   - onConnect and onDisconnect, after the pairing owner's, to the link
 *     observer (kLink). Never the security callbacks: Opera's object would
 *     answer them with the library's defaults, which is the defect.
 *
 * With no pairing owner (a build without the pairing channel, or before it
 * is up) a Numeric Comparison is answered no and the passkey shown is a
 * random one nobody sees: pairing fails closed instead of taking the
 * library's yes.
 *
 * Not overridden: onPassKeyEntry. NimBLE-Arduino 2.3.8, the floor
 * platformio.ini pins, does not declare it (2.5.0 does), and the WAP's IO
 * capability (DISPLAY_YESNO, bluetooth_channel.cpp) never asks this side to
 * type a passkey: the responder with a display shows it, the phone types.
 *
 * Header-only: the channel's .cpp and the sketch (which includes
 * ble_opera.h) share the one inline Dispatcher. Host-tested
 * (tests_host/test_bluetooth_commands_wap.cpp builds both inits through
 * the NimBLE stand-in); the Arduino compile is CI's; not bench-tested.
 */

#ifndef SECURACV_BLE_SERVER_DISPATCH_H
#define SECURACV_BLE_SERVER_DISPATCH_H

#include <Arduino.h>        // esp_random()
#include <NimBLEDevice.h>
#include <NimBLEServer.h>

#include <atomic>
#include <stdint.h>

namespace ble_server_dispatch {

// Who a set of server callbacks belongs to.
enum Role : uint8_t {
  kPairing = 0,   // every callback (the pairing channel)
  kLink,          // onConnect and onDisconnect only (Opera)
};

class Dispatcher final : public NimBLEServerCallbacks {
 public:
  // The owner of `role` (nullptr: none). The pointers are read on the
  // NimBLE host task while an init sets them on the bring-up worker or an
  // HTTP handler's task, so they are atomic.
  void set(Role role, NimBLEServerCallbacks* owner) {
    (role == kPairing ? pairing_ : link_).store(owner, std::memory_order_release);
  }
  NimBLEServerCallbacks* owner(Role role) const {
    return (role == kPairing ? pairing_ : link_).load(std::memory_order_acquire);
  }

  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) p->onConnect(server, connInfo);
    NimBLEServerCallbacks* l = owner(kLink);
    if (l != nullptr) l->onConnect(server, connInfo);
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) p->onDisconnect(server, connInfo, reason);
    NimBLEServerCallbacks* l = owner(kLink);
    if (l != nullptr) l->onDisconnect(server, connInfo, reason);
  }

  // The security callbacks: the pairing owner's alone, or failed closed.
  uint32_t onPassKeyDisplay() override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) return p->onPassKeyDisplay();
    // Not the library's 123456: six digits nobody is shown.
    return esp_random() % 1000000;
  }

  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) {
      p->onConfirmPassKey(connInfo, pin);
      return;
    }
    // No owner to show the six digits to: no (the library's default is yes).
    NimBLEDevice::injectConfirmPasskey(connInfo, false);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) p->onAuthenticationComplete(connInfo);
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) p->onMTUChange(mtu, connInfo);
  }

  void onIdentity(NimBLEConnInfo& connInfo) override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) p->onIdentity(connInfo);
  }

  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) p->onConnParamsUpdate(connInfo);
  }

  void onPhyUpdate(NimBLEConnInfo& connInfo, uint8_t txPhy, uint8_t rxPhy) override {
    NimBLEServerCallbacks* p = owner(kPairing);
    if (p != nullptr) p->onPhyUpdate(connInfo, txPhy, rxPhy);
  }

 private:
  std::atomic<NimBLEServerCallbacks*> pairing_{nullptr};
  std::atomic<NimBLEServerCallbacks*> link_{nullptr};
};

// The one Dispatcher (C++17 inline variable: one object for every
// translation unit that includes this header).
inline Dispatcher g_dispatcher;

// Records `owner` for `role` (nullptr clears it), then puts the Dispatcher
// on `server` as its callbacks. The one way a module installs server
// callbacks: firmware/scripts/check_wap_loop_commands.py (rule BD1) refuses
// a setCallbacks() of a server callbacks object anywhere else in the
// sketch. NimBLE must not delete the Dispatcher when the server goes
// (NimBLEDevice::deinit(true)): it is not on the heap, so the
// deleteCallbacks flag is false. False when there is no server.
inline bool install(NimBLEServer* server, Role role, NimBLEServerCallbacks* owner) {
  g_dispatcher.set(role, owner);
  if (server == nullptr) return false;
  server->setCallbacks(&g_dispatcher, false);
  return true;
}

}  // namespace ble_server_dispatch

#endif  // SECURACV_BLE_SERVER_DISPATCH_H
