#pragma once
// NimBLE-Arduino stand-in for test_improv_glue.cpp: the surface
// firmware/common/network/improv_ble.cpp calls, nothing more, in the shape of
// NimBLE-Arduino 2.5.0 when ESP_ARDUINO_VERSION_MAJOR >= 3 and 1.4.3 below.
// The envs that compile the glue float their pins (canary-sense.ini
// h2zero/NimBLE-Arduino@^2.3.8 on core 3, canary-vision.ini ~1.4.3 on core
// 2), so CI builds the newest release each admits; 2.5.0 and 1.4.3 are the
// releases these signatures were checked against. Names, parameter lists,
// return types and the callbacks each release calls (and in what order) are
// copied from those releases' headers and their gap-event handlers, so the
// glue's calls meet each release's signatures here too; the compile against
// the real library is still the family envs' in CI. (2.3.8 and 2.3.9, which
// ^2.3.8 also admits, declare NimBLEServer::start() void; the glue tests its
// result on core 3, so it builds on 2.4.0 and later only.)
//
// What the glue branches on is the test's to set: the connect handle, the
// encryption bit, the disconnect reason, whether NimBLE is up, whether
// init() brings it up, the server's start verdict (2.x) and each service's
// (1.4.x), through the g_* members below and the descriptors it hands the
// callbacks. A value a release fixes is fixed here the same way (2.5.0's
// NimBLEService::start() answers true; getAdvertising(), createServer(),
// createService() and createCharacteristic() allocate and never answer null
// on either). disconnect() asks the stack to drop a link and returns at once,
// as ble_gap_terminate() does; the event that says the link is gone comes
// later on the host task, so the test delivers it (test_improv_glue.cpp's
// host_turn()). What a characteristic was given (its value, each
// notification) and what went on air are kept for the test to read.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
#ifndef ESP_ARDUINO_VERSION_MAJOR
#error "set ESP_ARDUINO_VERSION_MAJOR (3: NimBLE-Arduino 2.x; 2: 1.4.x)"
#endif

// host/ble_gap.h, host/ble_hs_adv.h, host/ble_hs.h, nimble/ble.h, host/ble_att.h
#define BLE_GAP_CONN_MODE_NON 0
#define BLE_GAP_CONN_MODE_DIR 1
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_HS_ADV_F_DISC_GEN 0x02
#define BLE_HS_ADV_F_BREDR_UNSUP 0x04
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_ATT_ATTR_MAX_LEN 512

// NimBLECharacteristic.h (1.4.3) / NimBLELocalValueAttribute.h (2.5.0): an
// unscoped enum the glue ORs into a uint32_t, values from host/ble_gatt.h.
typedef enum {
  READ = 0x0002,
  READ_ENC = 0x0200,
  READ_AUTHEN = 0x0400,
  READ_AUTHOR = 0x0800,
  WRITE = 0x0008,
  WRITE_NR = 0x0004,
  WRITE_ENC = 0x1000,
  WRITE_AUTHEN = 0x2000,
  WRITE_AUTHOR = 0x4000,
  BROADCAST = 0x0001,
  NOTIFY = 0x0010,
  INDICATE = 0x0020
} NIMBLE_PROPERTY;

// host/ble_gap.h: the fields the glue (and NimBLEConnInfo) read.
struct ble_gap_sec_state {
  unsigned encrypted : 1;
  unsigned authenticated : 1;
  unsigned bonded : 1;
  unsigned key_size : 5;
  unsigned authorize : 1;
};
struct ble_gap_conn_desc {
  struct ble_gap_sec_state sec_state;
  uint16_t conn_handle;
};

// NimBLEConnInfo.h: a view of the descriptor, the same accessors.
class NimBLEConnInfo {
 public:
  NimBLEConnInfo() { memset(&m_desc, 0, sizeof(m_desc)); }
  uint16_t getConnHandle() const { return m_desc.conn_handle; }
  bool isEncrypted() const { return (m_desc.sec_state.encrypted == 1); }
  ble_gap_conn_desc m_desc;
};

class NimBLEUUID {
 public:
  std::string s;
  NimBLEUUID(const char* c) : s(c ? c : "") {}
  NimBLEUUID(uint16_t v) {
    char b[8];
    snprintf(b, sizeof b, "%04x", v);
    s = b;
  }
};

// NimBLEAttValue.h: what getValue() answers (length() is a uint16_t).
class NimBLEAttValue {
 public:
  NimBLEAttValue() {}
  explicit NimBLEAttValue(const std::string& v) : m_v(v) {}
  uint16_t length() const { return (uint16_t)m_v.size(); }
  uint16_t size() const { return (uint16_t)m_v.size(); }
  const uint8_t* data() const { return (const uint8_t*)m_v.data(); }

 private:
  std::string m_v;
};

class NimBLECharacteristic;
class NimBLECharacteristicCallbacks {
 public:
  virtual ~NimBLECharacteristicCallbacks() {}
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  virtual void onRead(NimBLECharacteristic*, NimBLEConnInfo&) {}
  virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
  virtual void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t) {}
#else
  virtual void onRead(NimBLECharacteristic*) {}
  virtual void onRead(NimBLECharacteristic*, ble_gap_conn_desc*) {}
  virtual void onWrite(NimBLECharacteristic*) {}
  virtual void onWrite(NimBLECharacteristic*, ble_gap_conn_desc*) {}
  virtual void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc*, uint16_t) {}
#endif
};

class NimBLECharacteristic {
 public:
  std::string uuid;
  uint32_t props = 0;
  NimBLECharacteristicCallbacks* cb = nullptr;
  std::string value;                    // what a read answers now
  std::vector<std::string> notified;    // the value at each notify()

  void setValue(const uint8_t* data, size_t size) { value.assign((const char*)data, size); }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  NimBLEAttValue getValue() const { return NimBLEAttValue(value); }
#else
  NimBLEAttValue getValue(time_t* /*timestamp*/ = nullptr) { return NimBLEAttValue(value); }
#endif
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  bool notify(uint16_t /*connHandle*/ = BLE_HS_CONN_HANDLE_NONE) const {
    const_cast<NimBLECharacteristic*>(this)->notified.push_back(value);
    return true;
  }
#else
  void notify(bool /*is_notification*/ = true) { notified.push_back(value); }
#endif
  void setCallbacks(NimBLECharacteristicCallbacks* c) { cb = c; }
};

class NimBLEService {
 public:
  std::string uuid;
  std::vector<NimBLECharacteristic*> chars;
  NimBLECharacteristic* createCharacteristic(const char* u, uint32_t properties = READ | WRITE,
                                             uint16_t /*max_len*/ = BLE_ATT_ATTR_MAX_LEN) {
    auto* c = new NimBLECharacteristic;
    c->uuid = u;
    c->props = properties;
    chars.push_back(c);
    return c;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  bool start() { return true; }         // 2.5.0: deprecated, always true
#else
  // 1.4.3 registers each service in its own start(), which can refuse: the
  // UUID whose start() the test refuses ("" = none).
  static inline std::string g_refuse_start;
  bool start() { return uuid != g_refuse_start; }
#endif
};

class NimBLEServer;
class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks() {}
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
  virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int /*reason*/) {}
  virtual void onMTUChange(uint16_t, NimBLEConnInfo&) {}
#else
  virtual void onConnect(NimBLEServer*) {}
  virtual void onConnect(NimBLEServer*, ble_gap_conn_desc*) {}
  virtual void onDisconnect(NimBLEServer*) {}
  virtual void onDisconnect(NimBLEServer*, ble_gap_conn_desc*) {}
  virtual void onMTUChange(uint16_t, ble_gap_conn_desc*) {}
#endif
};

class NimBLEServer {
 public:
  NimBLEServerCallbacks* cb = nullptr;
  std::vector<NimBLEService*> svcs;
  // Every handle disconnect() was asked to drop, in order; the test's
  // host_turn() delivers the stack's disconnect event for each live one.
  mutable std::vector<uint16_t> disconnects;
  bool started = false;

  void setCallbacks(NimBLEServerCallbacks* c, bool /*deleteCallbacks*/ = true) { cb = c; }
  NimBLEService* createService(const char* u) {
    auto* s = new NimBLEService;
    s->uuid = u;
    svcs.push_back(s);
    return s;
  }
  void removeService(NimBLEService* service, bool /*deleteSvc*/ = false) {
    for (size_t i = 0; i < svcs.size(); ++i)
      if (svcs[i] == service) { svcs.erase(svcs.begin() + (long)i); break; }
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  static inline bool g_start_ok = true;   // 2.5.0: false when ble_gatts_start refuses
  bool start() { started = g_start_ok; return g_start_ok; }
  bool disconnect(uint16_t h, uint8_t /*reason*/ = BLE_ERR_REM_USER_CONN_TERM) const {
    disconnects.push_back(h);
    return true;
  }
#else
  void start() { started = true; }      // 1.4.3: void
  int disconnect(uint16_t h, uint8_t /*reason*/ = BLE_ERR_REM_USER_CONN_TERM) {
    disconnects.push_back(h);
    return 0;
  }
#endif
};

// What went on air: the advertisement and scan response each start() put up.
class NimBLEAdvertisementData {
 public:
  std::string mfg, name, service_data_uuid, service_data;
  std::vector<std::string> services;
  uint8_t flags = 0;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  bool setManufacturerData(const std::string& d) { mfg = d; return true; }
  bool setFlags(uint8_t f) { flags = f; return true; }
  bool setName(const std::string& n, bool /*isComplete*/ = true) { name = n; return true; }
  bool addServiceUUID(const NimBLEUUID& u) { services.push_back(u.s); return true; }
  bool setCompleteServices(const NimBLEUUID& u) { services.assign(1, u.s); return true; }
  bool setServiceData(const NimBLEUUID& u, const std::string& d) {
    service_data_uuid = u.s;
    service_data = d;
    return true;
  }
#else
  void setManufacturerData(const std::string& d) { mfg = d; }
  void setFlags(uint8_t f) { flags = f; }
  void setName(const std::string& n) { name = n; }
  void addServiceUUID(const NimBLEUUID& u) { services.push_back(u.s); }
  void setCompleteServices(const NimBLEUUID& u) { services.assign(1, u.s); }
  void setServiceData(const NimBLEUUID& u, const std::string& d) {
    service_data_uuid = u.s;
    service_data = d;
  }
#endif
};

class NimBLEAddress;
class NimBLEAdvertising {
 public:
  int mode = -1;   // BLE_GAP_CONN_MODE_* of the last start()
  bool on = false;
  int starts = 0;
  NimBLEAdvertisementData adv, scan;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  bool start(uint32_t /*duration*/ = 0, const NimBLEAddress* /*dirAddr*/ = nullptr) {
    on = true;
    ++starts;
    return true;
  }
  bool stop() { on = false; return true; }
  bool setConnectableMode(uint8_t m) { mode = m; return true; }
  bool setAdvertisementData(const NimBLEAdvertisementData& d) { adv = d; return true; }
  bool setScanResponseData(const NimBLEAdvertisementData& d) { scan = d; return true; }
#else
  typedef std::function<void(NimBLEAdvertising*)> advCompleteCB_t;
  bool start(uint32_t /*duration*/ = 0, advCompleteCB_t /*cb*/ = nullptr, NimBLEAddress* /*dirAddr*/ = nullptr) {
    on = true;
    ++starts;
    return true;
  }
  bool stop() { on = false; return true; }
  void setAdvertisementType(uint8_t m) { mode = m; }
  // 1.4.3 takes these by non-const reference.
  void setAdvertisementData(NimBLEAdvertisementData& d) { adv = d; }
  void setScanResponseData(NimBLEAdvertisementData& d) { scan = d; }
#endif
};

class NimBLEDevice {
 public:
  static inline NimBLEServer* g_server = nullptr;
  static inline NimBLEAdvertising* g_adv = nullptr;
  static inline bool g_init = true;        // NimBLE already up (the beacon brought it up)
  static inline bool g_init_works = true;  // init() brings it up
  static inline std::string g_init_name;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  static bool init(const std::string& name) {
    g_init_name = name;
    g_init = g_init_works;
    return g_init;
  }
  static bool isInitialized() { return g_init; }
  static bool setMTU(uint16_t) { return true; }
#else
  static void init(const std::string& name) {
    g_init_name = name;
    g_init = g_init_works;
  }
  static bool getInitialized() { return g_init; }
  static int setMTU(uint16_t) { return 0; }
#endif
  static void setSecurityAuth(bool, bool, bool) {}
  static void setSecurityIOCap(uint8_t) {}
  static NimBLEAdvertising* getAdvertising() {
    if (!g_adv) g_adv = new NimBLEAdvertising;
    return g_adv;
  }
  static NimBLEServer* createServer() {
    if (!g_server) g_server = new NimBLEServer;
    return g_server;
  }
};
