Full-stack test: the real firmware (src/main.cpp, kwp.cpp, ebc430.cpp, log.cpp) built for a PC with mock
ESP8266 libraries (mock/), a simulated EBC430 ECU on a simulated K-line (harness.cpp), a bridge that serves
the real web pages and websocket (bridge.py, ports 80/81, control on 8099), and 32 browser scenarios driven by
Playwright (tests.py). Linux/WSL, run as root or with permission to bind ports 80/81:

  pip install websockets playwright && playwright install chromium
  g++ -std=c++17 -O1 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -Imock -I../../include \
      -I<path to ArduinoJson 6.21.5>/src harness.cpp ../../src/main.cpp ../../src/kwp.cpp \
      ../../src/ebc430.cpp ../../src/log.cpp -lpthread -o fw_native
  python3 bridge.py ../../data/www ./fw_native &
  python3 tests.py            (or: python3 tests.py 18 24  to run single scenarios)

results.txt = last run. What it cannot prove: the ECU model is built from the protocol, not from a real
EBC430; Wi-Fi, flash and the real ESP8266 timing are not simulated (ArduinoJson sizes are computed by the
library itself, so they are right on both). Car tests are still needed.
