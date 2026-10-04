/**
 * @file http_status_line.h
 * @brief The status line canary_wap.ino's http_send_error() sends for a
 *        code. Pure and host-tested (tests_host/test_mesh_commands_wap.cpp).
 *
 * httpd_resp_set_status() takes the whole line, so every code an error
 * answer can carry needs one here; a code missing from the table goes out as
 * "400 Bad Request". Before sweep F96 the table had only 400, 404 and 500,
 * so the audio self-test's 409 and the BLE chirp send's 503 went out as 400
 * (their bodies were right, and both web UIs read only the body). F96's mesh
 * answers for a command that did not run are 409 mesh_busy and 503
 * mesh_timeout (mesh_network::not_run_status), the PlatformIO tree's codes.
 */
#ifndef CANARY_WAP_HTTP_STATUS_LINE_H
#define CANARY_WAP_HTTP_STATUS_LINE_H

inline const char* http_status_line(int status_code) {
  switch (status_code) {
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default:  return "400 Bad Request";
  }
}

#endif  // CANARY_WAP_HTTP_STATUS_LINE_H
