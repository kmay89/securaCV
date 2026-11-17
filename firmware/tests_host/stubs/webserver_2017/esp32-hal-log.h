/* Host fake of arduino-esp32's log macros for test_webserver_boundary.cpp:
 * every argument is still evaluated (as a device build with logging on
 * would), and nothing is printed. */
#ifndef STUB_WEBSERVER_2017_ESP32_HAL_LOG_H
#define STUB_WEBSERVER_2017_ESP32_HAL_LOG_H
template <typename... A>
inline void stub_ws_log_discard(const char*, A&&...) {}
#define log_v(...) stub_ws_log_discard(__VA_ARGS__)
#define log_d(...) stub_ws_log_discard(__VA_ARGS__)
#define log_i(...) stub_ws_log_discard(__VA_ARGS__)
#define log_w(...) stub_ws_log_discard(__VA_ARGS__)
#define log_e(...) stub_ws_log_discard(__VA_ARGS__)
#endif
