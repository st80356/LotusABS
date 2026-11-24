#pragma once
#include "Arduino.h"
typedef enum { WStype_ERROR, WStype_DISCONNECTED, WStype_CONNECTED, WStype_TEXT, WStype_BIN } WStype_t;
class WebSocketsServer {
public:
  typedef std::function<void(uint8_t num, WStype_t type, uint8_t *payload, size_t length)> WebSocketServerEvent;
  WebSocketsServer(uint16_t) {}
  void begin() {}
  void onEvent(WebSocketServerEvent cb) { _cb = cb; }
  void enableHeartbeat(uint32_t, uint32_t, uint8_t) {}
  void loop();
  int connectedClients(bool ping = false);
  IPAddress remoteIP(uint8_t) { return IPAddress(192,168,1,2); }
  bool broadcastTXT(String &payload);
  bool sendTXT(uint8_t num, String &payload);
  WebSocketServerEvent _cb;
};
