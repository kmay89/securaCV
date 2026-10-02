/* Host stand-in for NimBLE-Arduino 2.x (the canary-wap Bluetooth channel
 * host harness, test_bluetooth_commands_wap.cpp, sweep F111): the classes
 * and calls bluetooth_channel.cpp and ble_standard_profiles.h make, with
 * just enough state to answer them. Nothing here is a radio. Every call
 * that would act on the radio or the bond store (advertising start/stop, a
 * scan start/stop, a disconnect, a passkey answer, a bond delete) is
 * recorded with the task the test is playing (host_sim::note), and the
 * passkey answers are kept (host_sim::passkey_answers) so a test can see one
 * pending pairing answered once. */
#ifndef STUB_BT_NIMBLE_DEVICE_H
#define STUB_BT_NIMBLE_DEVICE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Arduino.h"

struct ble_addr_t {
  uint8_t type;
  uint8_t val[6];
};

class NimBLEAddress {
 public:
  NimBLEAddress() { memset(&a_, 0, sizeof a_); }
  NimBLEAddress(const uint8_t* addr, uint8_t type) {
    memcpy(a_.val, addr, 6);
    a_.type = type;
  }
  const ble_addr_t* getBase() const { return &a_; }
  uint8_t getType() const { return a_.type; }
  std::string toString() const {
    char b[18];
    snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", a_.val[5], a_.val[4],
             a_.val[3], a_.val[2], a_.val[1], a_.val[0]);
    return b;
  }
 private:
  ble_addr_t a_;
};

class NimBLEUUID {
 public:
  NimBLEUUID(const char* s) : s_(s ? s : "") {}
  explicit NimBLEUUID(uint16_t u) {
    char b[8];
    snprintf(b, sizeof b, "%04x", (unsigned)u);
    s_ = b;
  }
  bool operator==(const NimBLEUUID& o) const { return s_ == o.s_; }
 private:
  std::string s_;
};

namespace NIMBLE_PROPERTY {
enum : uint16_t {
  READ = 0x0001, READ_ENC = 0x0002, READ_AUTHEN = 0x0004, READ_AUTHOR = 0x0008,
  WRITE = 0x0010, WRITE_NR = 0x0020, WRITE_ENC = 0x0040, WRITE_AUTHEN = 0x0080,
  WRITE_AUTHOR = 0x0100, BROADCAST = 0x0200, NOTIFY = 0x0400, INDICATE = 0x0800,
};
}  // namespace NIMBLE_PROPERTY

#define BLE_HS_IO_DISPLAY_YESNO 1

namespace host_sim {
// Heap copies of a NimBLEConnInfo alive (the pending Numeric Comparison's,
// sweep F143): one owner deletes each once, so it comes back to 0.
inline std::atomic<int> conn_heap{0};
}  // namespace host_sim

// One link's view, copied by value (bluetooth_channel.cpp keeps a heap copy
// of the one awaiting a Numeric Comparison answer).
class NimBLEConnInfo {
 public:
  static void* operator new(size_t n) {
    ++host_sim::conn_heap;
    return ::operator new(n);
  }
  static void operator delete(void* p) {
    if (p == nullptr) return;
    --host_sim::conn_heap;
    ::operator delete(p);
  }
  uint16_t handle = 1;
  NimBLEAddress address;
  bool encrypted = false, authenticated = false, bonded = false;
  uint16_t getConnHandle() const { return handle; }
  NimBLEAddress getAddress() const { return address; }
  bool isEncrypted() const { return encrypted; }
  bool isAuthenticated() const { return authenticated; }
  bool isBonded() const { return bonded; }
};

class NimBLECharacteristic;
class NimBLECharacteristicCallbacks {
 public:
  virtual ~NimBLECharacteristicCallbacks() = default;
  virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
  virtual void onRead(NimBLECharacteristic*, NimBLEConnInfo&) {}
};

class NimBLECharacteristic {
 public:
  void setCallbacks(NimBLECharacteristicCallbacks* cb) { cb_ = cb; }
  void setValue(const uint8_t* v, size_t n) { value_.assign((const char*)v, n); }
  void setValue(const char* v) { value_ = v ? v : ""; }
  std::string getValue() const { return value_; }
  void notify() { ++notifies; }
  unsigned notifies = 0;
 private:
  NimBLECharacteristicCallbacks* cb_ = nullptr;
  std::string value_;
};

class NimBLEService {
 public:
  NimBLECharacteristic* createCharacteristic(const NimBLEUUID&, uint32_t) {
    chars_.emplace_back(new NimBLECharacteristic());
    return chars_.back().get();
  }
  void start() {}
 private:
  std::vector<std::unique_ptr<NimBLECharacteristic>> chars_;
};

class NimBLEServer;
class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks() = default;
  virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
  virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
  virtual void onAuthenticationComplete(NimBLEConnInfo&) {}
  virtual uint32_t onPassKeyDisplay() { return 0; }
  virtual void onConfirmPassKey(NimBLEConnInfo&, uint32_t) {}
};

class NimBLEServer {
 public:
  void setCallbacks(NimBLEServerCallbacks* cb) { cb_ = cb; }
  NimBLEServerCallbacks* callbacks() const { return cb_; }
  NimBLEService* createService(const NimBLEUUID&) {
    services_.emplace_back(new NimBLEService());
    return services_.back().get();
  }
  bool updateConnParams(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) { return true; }
  std::vector<uint16_t> getPeerDevices() const { return peers; }
  bool disconnect(uint16_t) {
    host_sim::note("disconnect");
    return true;
  }
  std::vector<uint16_t> peers;   // the links up, as the test sets them
 private:
  NimBLEServerCallbacks* cb_ = nullptr;
  std::vector<std::unique_ptr<NimBLEService>> services_;
};

class NimBLEAdvertising {
 public:
  void addServiceUUID(const NimBLEUUID&) {}
  void setAppearance(uint16_t) {}
  // NimBLE's own state, which it keeps under its own lock (ble_gap's):
  // read and written atomically here, so the threaded test finds only the
  // channel's races.
  bool start() {
    host_sim::note("adv_start");
    __atomic_store_n(&advertising_, true, __ATOMIC_RELEASE);
    return true;
  }
  bool stop() {
    host_sim::note("adv_stop");
    __atomic_store_n(&advertising_, false, __ATOMIC_RELEASE);
    return true;
  }
  bool isAdvertising() const { return __atomic_load_n(&advertising_, __ATOMIC_ACQUIRE); }
 private:
  bool advertising_ = false;
};

class NimBLEAdvertisedDevice {
 public:
  NimBLEAddress address;
  int rssi = -60;
  std::string name;
  NimBLEAddress getAddress() const { return address; }
  int getRSSI() const { return rssi; }
  bool haveName() const { return !name.empty(); }
  std::string getName() const { return name; }
  bool isConnectable() const { return true; }
  bool isAdvertisingService(const NimBLEUUID&) const { return false; }
  bool haveAppearance() const { return false; }
  uint16_t getAppearance() const { return 0; }
};

class NimBLEScanResults {};

class NimBLEScanCallbacks {
 public:
  virtual ~NimBLEScanCallbacks() = default;
  virtual void onResult(const NimBLEAdvertisedDevice*) {}
  virtual void onScanEnd(const NimBLEScanResults&, int) {}
};

class NimBLEScan {
 public:
  void setScanCallbacks(NimBLEScanCallbacks* cb, bool = false) { cb_ = cb; }
  NimBLEScanCallbacks* callbacks() const { return cb_; }
  void setActiveScan(bool) {}
  void setInterval(uint16_t) {}
  void setWindow(uint16_t) {}
  bool start(uint32_t, bool = false, bool = true) {
    host_sim::note("scan_start");
    __atomic_store_n(&scanning_, true, __ATOMIC_RELEASE);
    return true;
  }
  bool stop() {
    host_sim::note("scan_stop");
    __atomic_store_n(&scanning_, false, __ATOMIC_RELEASE);
    return true;
  }
  bool isScanning() const { return __atomic_load_n(&scanning_, __ATOMIC_ACQUIRE); }
 private:
  NimBLEScanCallbacks* cb_ = nullptr;
  bool scanning_ = false;
};

namespace host_sim {
struct PasskeyAnswer {
  uint16_t handle;
  bool accept;
  std::string task;
};
inline std::mutex passkey_mu;
inline std::vector<PasskeyAnswer> passkey_answers;
inline bool nimble_up = false;
inline std::unique_ptr<NimBLEServer> server;
inline NimBLEAdvertising advertising;
inline NimBLEScan scan;
inline std::vector<NimBLEAddress> bonds;
}  // namespace host_sim

class NimBLEDevice {
 public:
  static bool isInitialized() { return host_sim::nimble_up; }
  static bool init(const std::string&) {
    host_sim::note("nimble_init");
    host_sim::nimble_up = true;
    return true;
  }
  static bool deinit(bool) {
    host_sim::nimble_up = false;
    host_sim::server.reset();
    return true;
  }
  static bool setPower(int8_t) {
    host_sim::note("set_power");
    return true;
  }
  static bool setMTU(uint16_t) { return true; }
  static bool setDefaultPhy(uint8_t, uint8_t) { return true; }
  static void setSecurityAuth(bool, bool, bool) {}
  static void setSecurityIOCap(uint8_t) {}
  static NimBLEServer* createServer() {
    if (!host_sim::server) host_sim::server.reset(new NimBLEServer());
    return host_sim::server.get();
  }
  static NimBLEServer* getServer() { return host_sim::server.get(); }
  static NimBLEAdvertising* getAdvertising() { return &host_sim::advertising; }
  static NimBLEScan* getScan() { return &host_sim::scan; }
  static bool injectConfirmPasskey(const NimBLEConnInfo& c, bool accept) {
    host_sim::note("passkey_answer");
    std::lock_guard<std::mutex> g(host_sim::passkey_mu);
    host_sim::passkey_answers.push_back({c.getConnHandle(), accept, host_sim::task});
    return true;
  }
  static bool deleteBond(const NimBLEAddress&) {
    host_sim::note("bond_delete");
    return true;
  }
  static int getNumBonds() { return (int)host_sim::bonds.size(); }
  static NimBLEAddress getBondedAddress(int i) { return host_sim::bonds[(size_t)i]; }
  static NimBLEAddress getAddress() { return NimBLEAddress(); }
};

// NimBLE's host C API, the calls update() and onConnect() make.
inline int ble_gap_set_prefered_le_phy(uint16_t, uint8_t, uint8_t, uint16_t) { return 0; }
inline int ble_gap_conn_rssi(uint16_t, int8_t* out) {
  *out = -50;
  return 0;
}
inline uint16_t ble_att_mtu(uint16_t) { return 247; }

#endif
