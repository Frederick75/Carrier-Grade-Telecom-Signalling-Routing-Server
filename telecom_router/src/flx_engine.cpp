#include "alr_store.hpp"
#include "ipc_mq.hpp"
#include "protocol.hpp"

#include <atomic>
#include <csignal>

using namespace tr;

static std::atomic<bool> g_run{true};
static void on_sig(int) { g_run = false; }

static std::string json_get_string(const std::string& j, const std::string& key) {
  // Minimal JSON extraction for demo (production: use a JSON lib like RapidJSON)
  // expects: "key":"value"
  const std::string pat = "\"" + key + "\"";
  auto k = j.find(pat);
  if (k == std::string::npos) return {};
  auto colon = j.find(':', k + pat.size());
  if (colon == std::string::npos) return {};
  auto q1 = j.find('"', colon);
  if (q1 == std::string::npos) return {};
  auto q2 = j.find('"', q1 + 1);
  if (q2 == std::string::npos) return {};
  return j.substr(q1 + 1, q2 - (q1 + 1));
}

int main() {
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  const std::string REQ  = "/tr_mq_req";
  const std::string RESP = "/tr_mq_resp";

  // Engine creates queues (server opens without create)
  PosixMq mq_req, mq_resp;
  mq_req.open(MqConfig{REQ, 2048, 8192, true, false});
  mq_resp.open(MqConfig{RESP, 2048, 8192, true, false});

  log_info("FLX engine started. MQ REQ=" + REQ + " RESP=" + RESP);

  AlrStore alr;
  std::vector<uint8_t> buf(static_cast<size_t>(mq_req.msgsize()));

  while (g_run.load()) {
    ssize_t n = -1;
    try {
      n = mq_req.recv(buf.data(), buf.size(), nullptr); // blocking
    } catch (const std::exception& e) {
      log_err(std::string("mq recv error: ") + e.what());
      continue;
    }
    if (n <= 0) continue;

    MsgHdr h{};
    std::string payload;
    if (!unpack(buf.data(), static_cast<size_t>(n), h, payload)) {
      log_warn("bad message received");
      continue;
    }
    if (static_cast<MsgType>(h.type) != MsgType::RouteReq) {
      log_warn("unexpected msg type");
      continue;
    }

    const auto msisdn = json_get_string(payload, "msisdn");
    const auto op = json_get_string(payload, "op");

    // Simulate routing work + low latency decision
    const uint64_t t0 = steady_millis();

    std::ostringstream resp;
    resp << "{";
    resp << "\"corr_id\":" << h.corr_id << ",";
    resp << "\"op\":\"" << (op.empty() ? "route" : op) << "\",";
    resp << "\"msisdn\":\"" << msisdn << "\",";

    auto rec = alr.lookup_msisdn(msisdn);
    if (!rec) {
      resp << "\"status\":\"NOT_FOUND\",";
      resp << "\"reason\":\"subscriber_not_in_alr\"";
    } else {
      const auto rg = route_policy(*rec);
      resp << "\"status\":\"OK\",";
      resp << "\"imsi\":\"" << rec->imsi << "\",";
      resp << "\"serving_msc\":\"" << rec->serving_msc << "\",";
      resp << "\"serving_vlr\":\"" << rec->serving_vlr << "\",";
      resp << "\"route_group\":\"" << rg << "\"";
    }

    const uint64_t t1 = steady_millis();
    resp << ",\"flx_latency_ms\":" << (t1 - t0);
    resp << "}";

    auto out = pack(MsgType::RouteResp, h.corr_id, resp.str());
    try {
      (void)mq_resp.send(out.data(), out.size(), 0);
    } catch (const std::exception& e) {
      log_err(std::string("mq send error: ") + e.what());
    }
  }

  log_info("FLX engine stopping.");
  return 0;
}
