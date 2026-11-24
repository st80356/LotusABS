#pragma once
#include "Arduino.h"
#include <map>
#include <vector>
#include <string>
enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };
class ESP8266WebServer {
public:
  typedef std::function<void(void)> THandler;
  ESP8266WebServer(int) {}
  void serveStatic(const char*, ...) {}
  void on(const char *uri, HTTPMethod, THandler fn) { handlers[uri] = fn; }
  void on(const char *uri, THandler fn) { handlers[uri] = fn; }
  void on(const char *uri, HTTPMethod, THandler fn, THandler) { handlers[uri] = fn; }
  void begin() {}
  void handleClient();
  bool hasArg(const char *n) { return args.count(n) > 0; }
  String arg(const char *n) { return args.count(n) ? String(args[n]) : String(""); }
  void sendHeader(const char*, const char*) {}
  void send(int code, const char *ctype, const String &body);
  void send(int code, const char *ctype, const char *body) { send(code, ctype, String(body)); }
  bool authenticate(const char*, const char*) { return true; }
  void requestAuthentication() {}
  std::map<std::string, THandler> handlers;
  std::map<std::string, std::string> args;
  int cur_id = -1;
};
