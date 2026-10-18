// Host test only: forced into test_mqtt_reinit's build of csi_mqtt.cpp
// (`-include`, tests_host/Makefile), so wap_json_writer.h's test hook has a
// body there. GET /api/mqtt/config builds its answer twice, measuring and
// then writing, and the write pass refuses a string that grew after it was
// measured; nothing in csi_mqtt.cpp can grow one (both passes read copies),
// so test_mqtt_reinit.cpp reaches that refusal through this hook: it grows a
// string of the handler's own copy of the config inside the writing pass,
// and holds what the handler does then (a 500, and its buffer freed).
//
// Never part of a firmware build: wap_json_writer.h compiles the hook only
// when WAP_JSON_TEST_HOOK_AFTER_MEASURE is defined, and only this header
// defines it for csi_mqtt.cpp.

#ifndef CANARY_WAP_MQTT_TEST_HOOKS_H
#define CANARY_WAP_MQTT_TEST_HOOKS_H

namespace mqtt_test {
// Called with each string the writing pass has just measured; null to do nothing.
extern void (*after_measure)(const char* s);
}  // namespace mqtt_test

#define WAP_JSON_TEST_HOOK_AFTER_MEASURE(s)                       \
  do {                                                            \
    if (::mqtt_test::after_measure) ::mqtt_test::after_measure(s); \
  } while (0)

#endif  // CANARY_WAP_MQTT_TEST_HOOKS_H
