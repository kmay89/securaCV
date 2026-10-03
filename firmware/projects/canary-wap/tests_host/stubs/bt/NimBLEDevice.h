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
 * store is keyed by identity, as NimBLE's is (sweep F172).
 *
 * And deleteBond() answers as ble_gap_unpair() does (NimBLE-Arduino 2.3.8
 * and 2.5.0, ble_gap.c, the same file in both): it first ends a live link to
 * that address, then answers false for an address with no bond (the store's
 * BLE_HS_ENOENT), and false with the bond kept (BLE_HS_EBUSY) for a bond
 * that carries the peer's IRK (host_sim::store_bond(addr, true): a phone
 * that uses private addresses hands it over) while the advertiser runs or a
 * discovery does (this scanner's, or the presence loop's on the same
 * scanner, host_sim::presence_disc). Before the F172 review it deleted
 * every bond and answered true, so no test could see the refusal a device
 * meets. */
#ifndef STUB_BT_NIMBLE_DEVICE_H
#define STUB_BT_NIMBLE_DEVICE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <functional>
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
  std::vector<uint16_t> getPeerDevices() const {
    ++peer_devices_calls;
    return peers;
  }
  // The stack's own record of a link (ble_gap_conn_find): what is up on
  // `handle` now. NimBLE answers a handle with no link with an empty
  // NimBLEConnInfo (handle 0, address 00:00:00:00:00:00).
  NimBLEConnInfo getPeerInfoByHandle(uint16_t handle) const {
    ++peer_info_calls;
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
  // ble_gap_unpair()'s first step (ble_hs_conn_find_by_addr, then
  // ble_gap_terminate_with_conn): the link whose identity is `identity` is
  // ended. The termination is the stack's own and asynchronous (its
  // disconnect event follows, as the test plays it), so the link stays up
  // here; its handle is kept apart from disconnect()'s.
  void unpair_ends_link(const NimBLEAddress& identity) {
    std::lock_guard<std::mutex> g(links_mu_);
    for (const auto& kv : links_) {
      if (kv.second.getIdAddress() == identity) {
        host_sim::note("unpair_terminate");
        ended_by_unpair.push_back(kv.first);
        return;
      }
    }
  }
  std::vector<uint16_t> disconnected;   // disconnect()'s handles, in order
  std::vector<uint16_t> ended_by_unpair;   // the links deleteBond() ended, in order
  // How often the channel asked the stack for its links (a test can see a
  // pass that asks nothing).
  mutable std::atomic<unsigned> peer_devices_calls{0};
  mutable std::atomic<unsigned> peer_info_calls{0};
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
// The bonds that carry the peer's IRK (keyed as `bonds` is).
inline std::vector<NimBLEAddress> bond_irks;
// The presence loop's endless scan on the same NimBLE scanner (ble_presence,
// which the test plays): a discovery to ble_gap_disc_active() as much as
// the owner's scan is.
inline std::atomic<bool> presence_disc{false};
// What deleteBond() refused for a busy radio (BLE_HS_EBUSY), in order.
inline std::vector<NimBLEAddress> bonds_busy;
// Run as deleteBond() starts: a test plays another task acting in that
// window (Opera's onConnect restarting the advertiser on the NimBLE host
// task, a chirp).
inline std::function<void()> before_unpair;
inline bool has_irk(const NimBLEAddress& a) {
  return std::find(bond_irks.begin(), bond_irks.end(), a) != bond_irks.end();
}
// The stack stores a bond under `identity`; `irk` when the peer handed over
// its identity resolving key (a phone using resolvable private addresses).
inline void store_bond(const NimBLEAddress& identity, bool irk) {
  if (std::find(bonds.begin(), bonds.end(), identity) == bonds.end()) bonds.push_back(identity);
  if (irk && !has_irk(identity)) bond_irks.push_back(identity);
}
// ble_gap_adv_active() || ble_gap_disc_active().
inline bool radio_busy() {
  return advertising.isAdvertising() || scan.isScanning() || presence_disc.load();
}
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
  // ble_gap_unpair() (NimBLEDevice::deleteBond() is `== 0` of it): a live
  // link to the address is ended first, whatever follows. No bond under the
  // address (neither PEER_SEC nor OUR_SEC holds it): false, the store's
  // error. A bond that carries the peer's IRK while the advertiser or a
  // discovery runs: false, BLE_HS_EBUSY, and the bond stays (its IRK cannot
  // leave the controller's resolving list meanwhile). Otherwise the bond
  // goes: true.
  static bool deleteBond(const NimBLEAddress& a) {
    host_sim::note("bond_delete");
    host_sim::bonds_deleted.push_back(a);
    if (host_sim::before_unpair) host_sim::before_unpair();
    if (host_sim::server) host_sim::server->unpair_ends_link(a);
    if (!isBonded(a)) return false;
    if (host_sim::has_irk(a) && host_sim::radio_busy()) {
      host_sim::note("bond_delete_busy");
      host_sim::bonds_busy.push_back(a);
      return false;
    }
    host_sim::bonds.erase(std::remove(host_sim::bonds.begin(), host_sim::bonds.end(), a),
                          host_sim::bonds.end());
    host_sim::bond_irks.erase(
        std::remove(host_sim::bond_irks.begin(), host_sim::bond_irks.end(), a),
        host_sim::bond_irks.end());
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
