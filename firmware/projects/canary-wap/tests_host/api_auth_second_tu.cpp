// A second translation unit over the REAL api_auth.h, linked into
// test_receipt_grant (sweep F343). On the device canary_wap.ino,
// csi_integration.cpp and csi_mqtt.cpp each compile api_auth.h, and the
// lockout's table must be ONE for all of them: before F343 it was `static
// AuthState g_auth` in the header, one copy per file, so a strike recorded by
// one file's gate never held another file's. This unit defines no
// auth_request_source(); the one test_receipt_grant cuts out of the sketch
// serves both, as the sketch's single definition serves every file.

#include "Arduino.h"           // stubs/receipt_grant
#include "esp_http_server.h"   // stubs/receipt_grant
#include "api_auth.h"          // the REAL one

// api_auth_check_bearer(), called from this file.
bool second_tu_check_bearer(httpd_req_t* r, const char* t) { return api_auth_check_bearer(r, t); }
