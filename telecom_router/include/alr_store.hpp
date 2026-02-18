#pragma once
#include "common.hpp"
#include <optional>
#include <unordered_map>

namespace tr {

// simple in-memory ALR simulation
struct AlrRecord {
  std::string imsi;
  std::string serving_msc;
  std::string serving_vlr;
  std::string region;
};

class AlrStore {
public:
  AlrStore() {
    // seed demo data (extend in production by loading from a DB/cache layer)
    db_["+14085551234"]  = {"310150123456789", "MSC_DALLAS_01", "VLR_DAL_01", "US-SOUTH"};
    db_["+12125550123"]  = {"310150987654321", "MSC_NYC_01",    "VLR_NYC_01", "US-EAST"};
    db_["+442079460123"] = {"234150111222333", "MSC_LON_01",    "VLR_LON_01", "UK"};
  }

  std::optional<AlrRecord> lookup_msisdn(const std::string& msisdn) const {
    auto it = db_.find(msisdn);
    if (it == db_.end()) return std::nullopt;
    return it->second;
  }

private:
  std::unordered_map<std::string, AlrRecord> db_;
};

// Example FLX routing policy decision
inline std::string route_policy(const AlrRecord& rec) {
  // pretend policy (could include congestion, priority, roaming)
  if (rec.region == "US-EAST")  return "ROUTE_GROUP_EAST";
  if (rec.region == "US-SOUTH") return "ROUTE_GROUP_SOUTH";
  return "ROUTE_GROUP_INTL";
}

} // namespace tr
