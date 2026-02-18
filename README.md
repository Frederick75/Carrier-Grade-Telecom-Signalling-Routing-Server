# Telecom Routing Server (C++17) — epoll + POSIX mqueue + FLX simulator

See:
- **BUILD.md** — production-ready build/run guide
- **PROJECT_EXPLANATION.md** — detailed architecture and telecom mapping

## Quick start

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j

# Terminal 1
./flx_engine

# Terminal 2
./routing_server 0.0.0.0 5555

# Terminal 3
printf '{"msisdn":"+14085551234","op":"route"}\n' | nc 127.0.0.1 5555
```
