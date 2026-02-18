# Detailed Project Explanation — Carrier-Grade Telecom Routing Server (Alcatel 5070 style)

## 1) What this project is

This repository implements a **carrier-grade telecom signalling routing server** and a simulated **FLX routing engine**.
The goal is to model the pattern commonly used on platforms like **Alcatel-Lucent 5070 Signalling Server** where:

- A front-end server accepts client routing/location queries over TCP/IP
- Internal routing logic is handled by a dedicated engine (FLX)
- Subscriber location is resolved from an ALR/HLR-style database
- Components communicate using **asynchronous IPC** for scalability and resilience

This code is written in **C++17** and uses:

- **Non-blocking sockets + epoll** for scalable connection handling
- **Thread pool** for parallel request processing
- **POSIX Message Queues (mqueue)** for inter-process request/response IPC
- **Correlation IDs** to match responses back to requesting client transactions

---

## 2) Telecom problem being solved

In telecom networks, routing decisions must often be made in real time:

- Where is the subscriber currently registered? (VLR/MSC region)
- Which route group or signalling path should be used?
- What translation/interworking is required between domains?

Examples include:
- call setup routing
- SMS routing
- mobility/location queries (e.g., “where is MSISDN currently served?”)
- routing simulation for network validation and capacity tests

The architecture here supports these needs by splitting responsibilities:

- **Routing Server**: connection-heavy, IO-heavy, multiplexing clients
- **FLX Engine**: decision-heavy, database-heavy, policy-heavy

---

## 3) High-level architecture

### Processes
1. **routing_server**
   - Handles TCP client sessions
   - Parses newline-delimited JSON requests
   - Submits work to worker threads
   - Sends the request to FLX via MQ and waits for a correlated response
   - Sends response back to the client

2. **flx_engine**
   - Creates the MQ queues (request + response)
   - Receives requests from routing_server
   - Performs ALR lookup
   - Applies routing policy
   - Sends a response with the same correlation ID

### Data flow

```
 TCP client
    |
    v
routing_server (epoll, multi-client)
    |
    v
 MQ /tr_mq_req  (RouteReq, corr_id)
    |
    v
flx_engine (ALR lookup + FLX policy)
    |
    v
 MQ /tr_mq_resp (RouteResp, corr_id)
    |
    v
routing_server -> TCP client response
```

---

## 4) Why message-queue IPC (telecom rationale)

Telecom signalling systems prioritize:
- throughput
- deterministic latency
- fault isolation
- graceful overload handling

Using MQ between routing_server and flx_engine provides:

- **Asynchronous decoupling**: the front-end can keep accepting connections while FLX processes decisions.
- **Fault containment**: FLX can restart independently; routing_server can apply timeouts.
- **Backpressure**: MQ capacity limits act as a natural buffer under load.
- **Multi-process scaling**: FLX can be replicated (future enhancement) with sharded queues.

---

## 5) Protocols and message formats in this repo

### 5.1 TCP request framing
- One request = one line (`\\n`)
- Each line is JSON

Example:
```json
{"msisdn":"+14085551234","op":"route"}
```

### 5.2 MQ wire format
MQ messages are:
- a small **binary header** (`MsgHdr`)
- followed by JSON payload bytes

Header fields:
- `magic`: identifies message family
- `version`: allows upgrades
- `type`: request or response
- `corr_id`: correlation ID to match response
- `payload_len`: size of JSON payload

This simulates telecom internal “envelope + payload” patterns used for fast dispatch.

---

## 6) FLX engine simulation details

### 6.1 ALR simulation
`include/alr_store.hpp` simulates ALR (Application Location Register).
In production, ALR data would come from:
- replicated database / cache
- HLR/HSS query
- local in-memory subscriber cache

This repo uses a small in-memory map keyed by `msisdn`.

### 6.2 Routing policy
`route_policy()` models how FLX converts subscriber region into a route group.
In production, FLX would:
- evaluate route tables and priorities
- apply roaming rules
- handle congestion status
- do number translation / interworking

---

## 7) Routing server concurrency model

### 7.1 epoll event loop
The routing server uses:
- non-blocking sockets
- `epoll_wait()` to handle many concurrent connections efficiently

This avoids a “one thread per connection” design and scales to large client counts.

### 7.2 Worker thread pool
Each parsed request line is handed to the worker pool.

Worker responsibilities:
1. Pack request into MQ message (with corr_id)
2. Send to MQ with short retry (handles temporary MQ full)
3. Wait up to 500ms for FLX response (bounded)
4. Enqueue response to connection output queue

### 7.3 Response dispatcher thread
A dedicated thread reads `/tr_mq_resp` and:
- unpacks messages
- finds matching `Pending` object by correlation ID
- signals the waiting worker via condition_variable

This allows MQ receive to be centralized and efficient.

---

## 8) Telecom-grade behaviors included

### 8.1 Backpressure
If outstanding transactions exceed `MAX_PENDING`, the server replies:
```json
{"status":"BUSY","reason":"overload"}
```
This prevents unbounded memory growth and overload collapse.

### 8.2 Bounded waits
Workers wait up to 500ms for FLX response and return:
```json
{"status":"TIMEOUT","reason":"flx_no_response"}
```
This is critical in carrier-grade systems to avoid hung transactions.

### 8.3 Retry on MQ full
If MQ is momentarily full, the routing server retries sends briefly.
In production, this would be replaced by:
- adaptive retry/backoff
- priority queues
- multi-engine sharding

---

## 9) Mapping to your Alcatel 5070 project bullets

- **TCP socket scripting interface** → newline framed JSON over TCP, epoll-based
- **Async message queue architecture** → POSIX mqueue request/response channels
- **FLX integration layer** → `flx_engine` simulating routing decisions
- **ALR integration** → `AlrStore` simulating subscriber mobility store
- **Optimized multi-threaded performance** → epoll + worker pool + bounded waits
- **Testing & validation** → netcat-based test + deterministic responses

---

## 10) What to upgrade next (true production)

If you want this to be closer to a real telecom product, upgrade in this order:

1. **Real JSON parser** (RapidJSON) + strict request schema validation
2. **eventfd-based wakeups** and thread-safe per-connection output queues (remove coarse mutex)
3. **Metrics** (TPS, p99) + tracing correlation across layers
4. **Multiple FLX instances** with sharded queues and load balancing
5. **HA**: warm restart, replay buffers, durable transaction logs if required
6. **SIGTRAN front-end** (SCTP/M3UA) to mimic SS7-over-IP ingress
7. **ALR backend**: Redis / replicated DB / in-memory cache with refresh policies

---

## 11) Quick demo commands

```bash
# Build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j

# Run in two terminals
./flx_engine
./routing_server 0.0.0.0 5555

# Test
printf '{"msisdn":"+14085551234","op":"route"}\n' | nc 127.0.0.1 5555
```

---

## 12) License and intended usage

This is a reference implementation for architecture and code structure.
For commercial deployment, add:
- full protocol definitions
- security (TLS/mTLS, authn/z)
- input validation + fuzz testing
- operational tooling and observability
