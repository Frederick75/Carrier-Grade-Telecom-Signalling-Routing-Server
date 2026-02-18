# Build & Run Guide — Telecom Routing Server (C++17, epoll, POSIX mqueue)

This repository contains a production-style **carrier-grade telecom routing server** and a simulated **FLX routing engine**.
It demonstrates a scalable architecture using:

- **Non-blocking TCP** front-end (Linux `epoll`)
- **Multi-threaded request processing** (thread pool)
- **IPC over POSIX Message Queues** (`mqueue`, `librt`)
- **Correlation ID** request/response matching
- **Backpressure & bounded wait** to prevent overload cascades

> **Processes**
>
> - `flx_engine`: receives routing requests from MQ, performs ALR lookup + routing decision, returns response via MQ
> - `routing_server`: accepts TCP clients, forwards requests to MQ, waits for correlated response, returns result to client


---

## 1. Prerequisites

### OS
- Linux (recommended)
  - POSIX message queues require `/dev/mqueue` mounted (typical on modern distros).

### Toolchain
- CMake 3.16+
- GCC 9+ or Clang 10+
- pthreads
- librt (`-lrt`) for mqueue

On Debian/Ubuntu:
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
```

On RHEL/CentOS:
```bash
sudo yum groupinstall -y "Development Tools"
sudo yum install -y cmake
```

---

## 2. Build

From repository root:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```

Artifacts:
- `build/routing_server`
- `build/flx_engine`

---

## 3. Run

### 3.1 Start FLX engine (creates MQ queues)

Terminal 1:
```bash
cd build
./flx_engine
```

Expected:
- FLX engine starts and creates `/tr_mq_req` and `/tr_mq_resp`

### 3.2 Start routing server (opens MQ queues, listens on TCP)

Terminal 2:
```bash
cd build
./routing_server 0.0.0.0 5555
```

---

## 4. Test with netcat

Terminal 3:
```bash
printf '{"msisdn":"+14085551234","op":"route"}\n' | nc 127.0.0.1 5555
```

Example response:
```json
{"corr_id":1,"op":"route","msisdn":"+14085551234","status":"OK","imsi":"310150123456789","serving_msc":"MSC_DALLAS_01","serving_vlr":"VLR_DAL_01","route_group":"ROUTE_GROUP_SOUTH","flx_latency_ms":0}
```

Try an unknown subscriber:
```bash
printf '{"msisdn":"+19998887777","op":"route"}\n' | nc 127.0.0.1 5555
```

---

## 5. Message framing & payload format

### TCP protocol
- Requests are **newline-delimited** JSON documents.
- One request per line.

Example request:
```json
{"msisdn":"+14085551234","op":"route"}
```

### MQ payload
- MQ messages use a small binary header (`include/protocol.hpp`) followed by the same JSON payload.
- Correlation is done using `corr_id` in the MQ header.

---

## 6. Troubleshooting

### 6.1 "mq_open failed" / `/dev/mqueue` missing
Ensure the mqueue filesystem is mounted:

```bash
mount | grep mqueue || sudo mount -t mqueue none /dev/mqueue
```

To make it persistent (systemd distros), enable with fstab if required.

### 6.2 MQ queue names already exist
If you previously ran the engine and it crashed, stale queues may remain.
You can remove them manually:

```bash
# View queues
ls -l /dev/mqueue

# Remove queues (names without leading slash in filesystem)
sudo rm -f /dev/mqueue/tr_mq_req /dev/mqueue/tr_mq_resp
```

Then restart `./flx_engine`.

### 6.3 Under load: "mq_full"
The routing server retries briefly when MQ is full. For heavier load:
- increase `mq_maxmsg` in `flx_engine` creation config, and/or
- increase message queue limits (`/proc/sys/fs/mqueue/*`), and/or
- scale FLX engine (multiple instances + sharded queues) in production.

---

## 7. Production hardening checklist (recommended next steps)

- Replace minimal JSON extraction with **RapidJSON** and schema validation
- Add **structured logging** and **metrics** (TPS, p99 latency)
- Replace coarse socket write mutex with **eventfd wakeups** and per-connection queues
- Add **circuit breakers** and **priority scheduling**
- Add **health checks** and watchdog integration
- Implement persistence/replication for ALR data
- Support SCTP/SIGTRAN front-end (M3UA) if needed

---

## 8. Repository map

- `src/routing_server.cpp` — epoll-based TCP server + thread pool + MQ request forwarding
- `src/flx_engine.cpp` — FLX simulator + ALR lookup + MQ response publishing
- `include/ipc_mq.hpp` — POSIX mqueue wrapper
- `include/protocol.hpp` — MQ wire header + pack/unpack helpers
- `include/thread_pool.hpp` — worker pool
- `include/alr_store.hpp` — ALR simulation store + routing policy
