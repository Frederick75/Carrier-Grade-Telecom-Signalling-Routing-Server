#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tr {

// MQ message type
enum class MsgType : uint16_t {
  RouteReq  = 1,
  RouteResp = 2
};

#pragma pack(push, 1)
struct MsgHdr {
  uint32_t magic{0x54524D51};   // 'TRMQ'
  uint16_t version{1};
  uint16_t type{0};
  uint64_t corr_id{0};
  uint32_t payload_len{0};
  uint32_t reserved{0};
};
#pragma pack(pop)

inline std::vector<uint8_t> pack(MsgType t, uint64_t corr_id, const std::string& payload) {
  MsgHdr h;
  h.type = static_cast<uint16_t>(t);
  h.corr_id = corr_id;
  h.payload_len = static_cast<uint32_t>(payload.size());

  std::vector<uint8_t> out(sizeof(MsgHdr) + payload.size());
  std::memcpy(out.data(), &h, sizeof(MsgHdr));
  if (!payload.empty()) {
    std::memcpy(out.data() + sizeof(MsgHdr), payload.data(), payload.size());
  }
  return out;
}

inline bool unpack(const uint8_t* data, size_t len, MsgHdr& h, std::string& payload) {
  if (len < sizeof(MsgHdr)) return false;
  std::memcpy(&h, data, sizeof(MsgHdr));
  if (h.magic != 0x54524D51 || h.version != 1) return false;
  if (sizeof(MsgHdr) + h.payload_len != len) return false;
  payload.assign(reinterpret_cast<const char*>(data + sizeof(MsgHdr)), h.payload_len);
  return true;
}

} // namespace tr
