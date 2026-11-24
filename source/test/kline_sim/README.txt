K-line simulator: runs the real src/kwp.cpp (v1.1, or v1.0 from "v1.0 original" source) against a
simulated EBC430 ABS ECU on a PC. Tests connect, DTCs, valves/pump, corrupt/noisy/slow replies,
ECU busy, dead L9637, ignition off, request order, and memory use over 2 hours.

Build and run (Linux / WSL, g++):
  g++ -std=c++17 -DV11 -include functional -Imock -I../../include sim.cpp ../../src/kwp.cpp -Wl,--wrap=malloc,--wrap=free -o sim && ./sim
  for v1.0 code: -DV10 and point -I / the kwp.cpp path at the v1.0 source.

results.txt = output for both versions. The ECU model is built from the protocol, not from a real
car: it cannot prove either version works on a real EBC430.
