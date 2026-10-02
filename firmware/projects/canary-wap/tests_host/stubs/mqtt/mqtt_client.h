/* ESP-IDF's mqtt_client.h for the MQTT bridge host build: the types and
 * calls csi_mqtt.cpp makes, declared only. test_mqtt_reinit.cpp defines
 * them as a fake client that records which task made each call. Plain C
 * declarations: csi_mqtt.cpp includes this inside extern "C". */
#ifndef STUB_MQTT_MQTT_CLIENT_H
#define STUB_MQTT_MQTT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif

typedef const char* esp_event_base_t;
typedef void (*esp_event_handler_t)(void* handler_args, esp_event_base_t base,
                                    int32_t event_id, void* event_data);
#define ESP_EVENT_ANY_ID (-1)

typedef enum {
  MQTT_EVENT_ANY = -1,
  MQTT_EVENT_ERROR = 0,
  MQTT_EVENT_CONNECTED,
  MQTT_EVENT_DISCONNECTED,
  MQTT_EVENT_SUBSCRIBED,
  MQTT_EVENT_UNSUBSCRIBED,
  MQTT_EVENT_PUBLISHED,
  MQTT_EVENT_DATA,
  MQTT_EVENT_BEFORE_CONNECT,
  MQTT_EVENT_DELETED,
} esp_mqtt_event_id_t;

typedef enum {
  MQTT_ERROR_TYPE_NONE = 0,
  MQTT_ERROR_TYPE_TCP_TRANSPORT,
  MQTT_ERROR_TYPE_CONNECTION_REFUSED,
} esp_mqtt_error_type_t;

typedef struct {
  esp_err_t esp_tls_last_esp_err;
  int esp_tls_stack_err;
  int esp_tls_cert_verify_flags;
  esp_mqtt_error_type_t error_type;
} esp_mqtt_error_codes_t;

typedef struct esp_mqtt_client* esp_mqtt_client_handle_t;

typedef struct {
  esp_mqtt_event_id_t event_id;
  esp_mqtt_client_handle_t client;
  char* data;
  int data_len;
  char* topic;
  int topic_len;
  esp_mqtt_error_codes_t* error_handle;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t* esp_mqtt_event_handle_t;

typedef struct {
  struct {
    struct { const char* uri; } address;
    struct { const char* certificate; } verification;
  } broker;
  struct {
    const char* username;
    struct { const char* password; } authentication;
  } credentials;
  struct {
    struct {
      const char* topic;
      const char* msg;
      int msg_len;
      int qos;
      int retain;
    } last_will;
    int keepalive;
  } session;
  /* IDF 5's network_t, the fields csi_mqtt.cpp may set (sweep F112):
   * timeout_ms is how long esp_mqtt lets one socket operation go without
   * progress; 0 or less means its default, 10 s. */
  struct {
    int reconnect_timeout_ms;
    int timeout_ms;
  } network;
} esp_mqtt_client_config_t;

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t* config);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t event,
                                         esp_event_handler_t handler, void* handler_args);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char* topic, const char* data,
                            int len, int qos, int retain);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char* topic, int qos);

#endif
