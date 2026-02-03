/**
 * @file http_server.h
 * @brief HTTP REST API server
 *
 * Provides HTTP server functionality for local device access.
 * Supports REST API endpoints, static file serving, and WebSocket.
 */

#pragma once

#include "../core/types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

#define HTTP_PORT_DEFAULT       80
#define HTTP_MAX_URI_LEN        128
#define HTTP_MAX_HANDLERS       64
#define HTTP_MAX_HEADER_LEN     256
#define HTTP_CHUNK_SIZE         4096

// ============================================================================
// TYPES
// ============================================================================

/**
 * @brief HTTP method
 */
typedef enum {
    HTTP_GET = 0,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_OPTIONS,
    HTTP_HEAD,
} http_method_t;

/**
 * @brief HTTP content type
 */
typedef enum {
    CONTENT_TYPE_TEXT_PLAIN = 0,
    CONTENT_TYPE_TEXT_HTML,
    CONTENT_TYPE_TEXT_CSS,
    CONTENT_TYPE_TEXT_JS,
    CONTENT_TYPE_JSON,
    CONTENT_TYPE_OCTET_STREAM,
    CONTENT_TYPE_MULTIPART,
    CONTENT_TYPE_JPEG,
} http_content_type_t;

/**
 * @brief HTTP request info
 */
typedef struct {
    http_method_t method;
    char uri[HTTP_MAX_URI_LEN];
    char query[HTTP_MAX_URI_LEN];
    http_content_type_t content_type;
    size_t content_length;
    void* user_ctx;
} http_request_t;

/**
 * @brief HTTP response helper
 */
typedef struct http_response http_response_t;

/**
 * @brief HTTP handler callback
 * @param req Request info
 * @param body Request body (may be NULL)
 * @param body_len Body length
 * @return HTTP status code
 */
typedef int (*http_handler_t)(
    const http_request_t* req,
    const uint8_t* body,
    size_t body_len,
    void* user_data
);

/**
 * @brief HTTP server status
 */
typedef struct {
    bool running;
    uint16_t port;
    uint32_t requests_total;
    uint32_t requests_ok;
    uint32_t requests_error;
    uint8_t active_connections;
    uint32_t uptime_ms;
} http_server_status_t;

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * @brief HTTP server configuration
 */
typedef struct {
    uint16_t port;
    uint8_t max_connections;
    bool enable_cors;
    bool enable_auth;
    const char* auth_user;
    const char* auth_pass;
    const char* server_name;
} http_server_config_t;

// Default configuration
#define HTTP_SERVER_CONFIG_DEFAULT { \
    .port = 80, \
    .max_connections = 4, \
    .enable_cors = true, \
    .enable_auth = false, \
    .auth_user = NULL, \
    .auth_pass = NULL, \
    .server_name = "SecuraCV", \
}

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize HTTP server
 * @param config Configuration
 * @return RESULT_OK on success
 */
result_t http_server_init(const http_server_config_t* config);

/**
 * @brief Deinitialize HTTP server
 * @return RESULT_OK on success
 */
result_t http_server_deinit(void);

/**
 * @brief Start HTTP server
 * @return RESULT_OK on success
 */
result_t http_server_start(void);

/**
 * @brief Stop HTTP server
 * @return RESULT_OK on success
 */
result_t http_server_stop(void);

/**
 * @brief Get HTTP server status
 * @param status Output status
 * @return RESULT_OK on success
 */
result_t http_server_get_status(http_server_status_t* status);

// ============================================================================
// ROUTE REGISTRATION
// ============================================================================

/**
 * @brief Register HTTP handler
 * @param method HTTP method
 * @param uri URI pattern (may include wildcards)
 * @param handler Handler callback
 * @param user_data User context
 * @return RESULT_OK on success
 */
result_t http_server_register(
    http_method_t method,
    const char* uri,
    http_handler_t handler,
    void* user_data
);

/**
 * @brief Unregister HTTP handler
 * @param method HTTP method
 * @param uri URI pattern
 * @return RESULT_OK on success
 */
result_t http_server_unregister(http_method_t method, const char* uri);

// ============================================================================
// RESPONSE HELPERS
// ============================================================================

/**
 * @brief Send JSON response
 * @param req Request (contains response context)
 * @param status HTTP status code
 * @param json JSON string
 * @return RESULT_OK on success
 */
result_t http_respond_json(const http_request_t* req, int status, const char* json);

/**
 * @brief Send plain text response
 * @param req Request
 * @param status HTTP status code
 * @param text Text content
 * @return RESULT_OK on success
 */
result_t http_respond_text(const http_request_t* req, int status, const char* text);

/**
 * @brief Send HTML response
 * @param req Request
 * @param status HTTP status code
 * @param html HTML content
 * @return RESULT_OK on success
 */
result_t http_respond_html(const http_request_t* req, int status, const char* html);

/**
 * @brief Send binary response
 * @param req Request
 * @param status HTTP status code
 * @param content_type Content type
 * @param data Binary data
 * @param len Data length
 * @return RESULT_OK on success
 */
result_t http_respond_binary(
    const http_request_t* req,
    int status,
    http_content_type_t content_type,
    const uint8_t* data,
    size_t len
);

/**
 * @brief Send error response
 * @param req Request
 * @param status HTTP error status
 * @param message Error message
 * @return RESULT_OK on success
 */
result_t http_respond_error(const http_request_t* req, int status, const char* message);

/**
 * @brief Start chunked transfer response
 * @param req Request
 * @param content_type Content type
 * @return RESULT_OK on success
 */
result_t http_respond_chunk_start(
    const http_request_t* req,
    http_content_type_t content_type
);

/**
 * @brief Send response chunk
 * @param req Request
 * @param data Chunk data
 * @param len Chunk length
 * @return RESULT_OK on success
 */
result_t http_respond_chunk(const http_request_t* req, const uint8_t* data, size_t len);

/**
 * @brief End chunked transfer
 * @param req Request
 * @return RESULT_OK on success
 */
result_t http_respond_chunk_end(const http_request_t* req);

// ============================================================================
// QUERY PARSING
// ============================================================================

/**
 * @brief Get query parameter value
 * @param req Request
 * @param key Parameter key
 * @param value Output buffer
 * @param max_len Buffer size
 * @return RESULT_OK if found
 */
result_t http_get_query_param(
    const http_request_t* req,
    const char* key,
    char* value,
    size_t max_len
);

/**
 * @brief Get query parameter as integer
 * @param req Request
 * @param key Parameter key
 * @param value Output value
 * @param default_val Default if not found
 * @return RESULT_OK if found
 */
result_t http_get_query_int(
    const http_request_t* req,
    const char* key,
    int* value,
    int default_val
);

// ============================================================================
// STANDARD API ENDPOINTS
// ============================================================================

/**
 * @brief Register standard API endpoints
 *
 * Registers the following endpoints:
 * - GET /api/status - Device status
 * - GET /api/health - Health metrics
 * - GET /api/config - Configuration
 * - POST /api/config - Update configuration
 * - GET /api/logs - Log export
 * - POST /api/logs/ack - Acknowledge logs
 * - GET /api/witness/export - Export witness records
 * - GET / - Web UI
 *
 * @return RESULT_OK on success
 */
result_t http_register_standard_api(void);

#ifdef __cplusplus
}
#endif
