/* Host stand-in for NimBLE-Arduino 2.x (the canary-wap Bluetooth channel
 * host harness, test_bluetooth_commands_wap.cpp, sweep F111): the classes
 * and calls bluetooth_channel.cpp and ble_standard_profiles.h make, with
 * just enough state to answer them. Nothing here is a radio. Every call
 * that would act on the radio or the bond store (advertising start/stop, a
 * scan start/stop, a disconnect, a passkey answer, a bond delete) is
 * recorded with the task the test is playing (host_sim::note), and the
 * passkey answers are kept (host_sim::passkey_answers) so a test can see one
 * pending pairing answered once. The server keeps the links the test says
 * are up (link_up), as the stack's getPeerInfoByHandle() answers them.
 *
 * Since sweep F171 the server callbacks are NimBLE-Arduino 2.5.0's: every
 * virtual it declares, and its defaults (NimBLEServer.cpp): a server starts
 * with them, setCallbacks(nullptr) goes back to them, and the default
 * onConfirmPassKey answers yes, onPassKeyDisplay shows 123456. So a test
 * that installs a second module's callbacks over the channel's sees what a
 * device would: the library's yes. setCallbacks' deleteCallbacks flag is
 * kept (callbacks_deleted_with_server()), where NimBLE would delete the
 * object when the server goes. A link names its over-the-air address and its
 * identity address (getIdAddress(): the same unless the test gives it a
 * resolvable private address over an identity, as phones use); the bond
 * store is keyed by identity, as NimBLE's is (sweep F172). */
#ifndef STUB_BT_NIMBLE_DEVICE_H
#define STUB_BT_NIMBLE_DEVICE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
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

// As NimBLE-Arduino 2.5.0's (src/NimBLEAddress.cpp): the native form keeps
// the bytes least significant first (getBase()->val, as the stack hands
// them over), toString() prints them most significant first, and the
// byte-array constructor takes them in the printed order, reversing them
// into the native form (std::reverse_copy). A copy of getBase()->val put
// back through that constructor comes out reversed; the ble_addr_t
// constructor keeps them as they are.
class NimBLEAddress {
 public:
  NimBLEAddress() { memset(&a_, 0, sizeof a_); }
  NimBLEAddress(const ble_addr_t address) : a_(address) {}
  NimBLEAddress(const uint8_t* addr, uint8_t type) {
    for (int i = 0; i < 6; ++i) a_.val[i] = addr[5 - i];
    a_.type = type;
  }
  const ble_addr_t* getBase() const { return &a_; }
  uint8_t getType() const { return a_.type; }
  // A resolvable private address: random, the two most significant bits 01.
  bool isRpa() const { return a_.type == 1 && (a_.val[5] & 0xC0) == 0x40; }
  bool operator==(const NimBLEAddress& o) const {
    return a_.type == o.a_.type && memcmp(a_.val, o.a_.val, 6) == 0;
  }
  bool operator!=(const NimBLEAddress& o) const { return !(*this == o); }
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
  NimBLEAddress address;                 // over the air (peer_ota_addr)
  // The identity (peer_id_addr), when the test gives one apart from the
  // over-the-air address; NimBLE reports the same address for a peer that
  // uses no private address.
  NimBLEAddress id_address;
  bool id_known = false;
  bool encrypted = false, authenticated = false, bonded = false;
  uint16_t getConnHandle() const { return handle; }
  NimBLEAddress getAddress() const { return address; }
  NimBLEAddress getIdAddress() const { return id_known ? id_address : address; }
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
// NimBLE-Arduino 2.5.0's NimBLEServerCallbacks: the same virtuals, and its
// defaults (defined below NimBLEDevice, which they call).
class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks() = default;
  virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
  virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
  virtual void onMTUChange(uint16_t, NimBLEConnInfo&) {}
  virtual uint32_t onPassKeyDisplay();                     // default: 123456
  virtual void onPassKeyEntry(NimBLEConnInfo& connInfo);   // default: injects 123456
  virtual void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin);   // default: yes
  virtual void onAuthenticationComplete(NimBLEConnInfo&) {}
  virtual void onIdentity(NimBLEConnInfo&) {}
  virtual void onConnParamsUpdate(NimBLEConnInfo&) {}
  virtual void onPhyUpdate(NimBLEConnInfo&, uint8_t, uint8_t) {}
};

namespace host_sim {
inline NimBLEServerCallbacks default_server_callbacks;   // NimBLEServer.cpp's defaultCallbacks
}  // namespace host_sim

class NimBLEServer {
 public:
  void setCallbacks(NimBLEServerCallbacks* cb, bool deleteCallbacks = true) {
    if (cb != nullptr) {
      cb_ = cb;
      delete_cb_ = deleteCallbacks;
    } else {
      cb_ = &host_sim::default_server_callbacks;
      delete_cb_ = false;
    }
  }
  // What the stack calls (m_pServerCallbacks).
  NimBLEServerCallbacks* callbacks() const { return cb_; }
  // Whether NimBLE would delete the callbacks object with the server.
  bool callbacks_deleted_with_server() const { return delete_cb_; }
  NimBLEService* createService(const NimBLEUUID&) {
    services_.emplace_back(new NimBLEService());
    return services_.back().get();
  }
  bool updateConnParams(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {
    host_sim::note("conn_params");
    return true;
  }
  std::vector<uint16_t> getPeerDevices() const { return peers; }
  // The stack's own record of a link (ble_gap_conn_find): what is up on
  // `handle` now. NimBLE answers a handle with no link with an empty
  // NimBLEConnInfo (handle 0, address 00:00:00:00:00:00).
  NimBLEConnInfo getPeerInfoByHandle(uint16_t handle) const {
    std::lock_guard<std::mutex> g(links_mu_);
    const auto it = links_.find(handle);
    if (it != links_.end()) return it->second;
    NimBLEConnInfo none;
    none.handle = 0;
    return none;
  }
  // The test plays the stack: a link comes up on its handle, or goes.
  void link_up(const NimBLEConnInfo& c) {
    std::lock_guard<std::mutex> g(links_mu_);
    links_[c.getConnHandle()] = c;
  }
  void link_down(uint16_t handle) {
    std::lock_guard<std::mutex> g(links_mu_);
    links_.erase(handle);
  }
  bool disconnect(uint16_t handle) {
    host_sim::note("disconnect");
    std::lock_guard<std::mutex> g(links_mu_);
    disconnected.push_back(handle);
    return true;
  }
  std::vector<uint16_t> disconnected;   // disconnect()'s handles, in order
  std::vector<uint16_t> peers;   // the links up, as the test sets them
 private:
  mutable std::mutex links_mu_;
  std::map<uint16_t, NimBLEConnInfo> links_;
  NimBLEServerCallbacks* cb_ = &host_sim::default_server_callbacks;
  bool delete_cb_ = false;
  std::vector<std::unique_ptr<NimBLEService>> services_;
};

// What an advertisement carries (ble_opera.h builds its beacon with it).
class NimBLEAdvertisementData {
 public:
  bool setManufacturerData(const std::string& d) { mfg = d; return true; }
  bool setName(const std::string& n, bool = true) { name = n; return true; }
  bool addServiceUUID(const NimBLEUUID&) { return true; }
  std::string mfg, name;
};

class NimBLEAdvertising {
 public:
  void addServiceUUID(const NimBLEUUID&) {}
  void setAppearance(uint16_t) {}
  bool setName(const std::string&) { return true; }
  bool enableScanResponse(bool) { return true; }
  bool setAdvertisementData(const NimBLEAdvertisementData&) { return true; }
  bool setScanResponseData(const NimBLEAdvertisementData&) { return true; }
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
  bool connectable = true;
  std::vector<NimBLEUUID> services;   // the service UUIDs it advertises
  NimBLEAddress getAddress() const { return address; }
  int getRSSI() const { return rssi; }
  bool haveName() const { return !name.empty(); }
  std::string getName() const { return name; }
  bool isConnectable() const { return connectable; }
  bool isAdvertisingService(const NimBLEUUID& u) const {
    for (const NimBLEUUID& s : services) {
      if (s == u) return true;
    }
    return false;
  }
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
inline std::vector<NimBLEAddress> bonds_deleted;   // deleteBond's arguments, in order
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
  static bool injectPassKey(const NimBLEConnInfo&, uint32_t) {
    host_sim::note("passkey_entry");
    return true;
  }
  // ble_gap_unpair(): the bond keyed by this address goes, if there is
  // one. It answers 0 (true) when there was none, too (ble_store's delete
  // of a missing key is not an error), so the record of what was asked for
  // and what is left in host_sim::bonds is the test's evidence, not this.
  static bool deleteBond(const NimBLEAddress& a) {
    host_sim::note("bond_delete");
    host_sim::bonds_deleted.push_back(a);
    host_sim::bonds.erase(std::remove(host_sim::bonds.begin(), host_sim::bonds.end(), a),
                          host_sim::bonds.end());
    return true;
  }
  static bool isBonded(const NimBLEAddress& a) {
    return std::find(host_sim::bonds.begin(), host_sim::bonds.end(), a) != host_sim::bonds.end();
  }
  static int getNumBonds() { return (int)host_sim::bonds.size(); }
  static NimBLEAddress getBondedAddress(int i) { return host_sim::bonds[(size_t)i]; }
  static NimBLEAddress getAddress() { return NimBLEAddress(); }
};

// NimBLE-Arduino 2.5.0's default server callbacks (NimBLEServer.cpp).
inline uint32_t NimBLEServerCallbacks::onPassKeyDisplay() { return 123456; }
inline void NimBLEServerCallbacks::onPassKeyEntry(NimBLEConnInfo& connInfo) {
  NimBLEDevice::injectPassKey(connInfo, 123456);
}
inline void NimBLEServerCallbacks::onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t) {
  NimBLEDevice::injectConfirmPasskey(connInfo, true);
}

// NimBLE's host C API, the calls update() and a link's connect (applied on
// the loop task since F143) make.
inline int ble_gap_set_prefered_le_phy(uint16_t, uint8_t, uint8_t, uint16_t) {
  host_sim::note("le_phy");
  return 0;
}
inline int ble_gap_conn_rssi(uint16_t, int8_t* out) {
  *out = -50;
  return 0;
}
inline uint16_t ble_att_mtu(uint16_t) { return 247; }

#endif
