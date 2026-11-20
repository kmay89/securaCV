/* Host fake of libb64's encoder for test_webserver_boundary.cpp: WebServer.cpp's
 * basic-auth path links it; no test reaches that path. */
#ifndef STUB_WEBSERVER_2017_CENCODE_H
#define STUB_WEBSERVER_2017_CENCODE_H
inline int base64_encode_expected_len(int plaintext_len) { return 4 * ((plaintext_len + 2) / 3); }
inline int base64_encode_chars(const char*, int, char* out) { out[0] = '\0'; return 0; }
#endif
