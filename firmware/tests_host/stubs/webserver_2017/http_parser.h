/* Host fake of ESP-IDF's http_parser.h method map for test_webserver_boundary.cpp.
 * The first eight entries keep http_parser's numbers; PATCH, which the real map
 * numbers 28, is 8 here. WebServer only needs each enum value to equal its
 * position in the map, which a contiguous list gives. */
#ifndef STUB_WEBSERVER_2017_HTTP_PARSER_H
#define STUB_WEBSERVER_2017_HTTP_PARSER_H
#define HTTP_METHOD_MAP(XX) \
  XX(0, DELETE, DELETE)     \
  XX(1, GET, GET)           \
  XX(2, HEAD, HEAD)         \
  XX(3, POST, POST)         \
  XX(4, PUT, PUT)           \
  XX(5, CONNECT, CONNECT)   \
  XX(6, OPTIONS, OPTIONS)   \
  XX(7, TRACE, TRACE)       \
  XX(8, PATCH, PATCH)
enum http_method {
#define XX(num, name, string) HTTP_##name = num,
  HTTP_METHOD_MAP(XX)
#undef XX
};
#endif
