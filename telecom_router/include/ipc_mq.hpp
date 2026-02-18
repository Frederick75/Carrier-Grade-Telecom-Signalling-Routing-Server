#pragma once
#include "common.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tr {

struct MqConfig {
  std::string name;
  long maxmsg{1024};
  long msgsize{8192}; // must fit header+payload
  bool create{false};
  bool nonblock{false};
};

class PosixMq {
public:
  PosixMq() = default;
  ~PosixMq() { close(); }

  PosixMq(const PosixMq&) = delete;
  PosixMq& operator=(const PosixMq&) = delete;

  void open(const MqConfig& cfg) {
    close();
    cfg_ = cfg;

    struct mq_attr attr{};
    attr.mq_flags = cfg.nonblock ? O_NONBLOCK : 0;
    attr.mq_maxmsg = cfg.maxmsg;
    attr.mq_msgsize = cfg.msgsize;

    int flags = O_RDWR;
    if (cfg.create) flags |= O_CREAT;
    if (cfg.nonblock) flags |= O_NONBLOCK;

    mqd_ = mq_open(cfg.name.c_str(), flags, 0660, cfg.create ? &attr : nullptr);
    if (mqd_ == (mqd_t)-1) {
      throw std::runtime_error("mq_open failed for " + cfg.name + ": " + std::string(std::strerror(errno)));
    }
  }

  void close() {
    if (mqd_ != (mqd_t)-1) {
      mq_close(mqd_);
      mqd_ = (mqd_t)-1;
    }
  }

  void unlink_queue() {
    if (!cfg_.name.empty()) {
      mq_unlink(cfg_.name.c_str());
    }
  }

  bool send(const uint8_t* data, size_t len, unsigned prio = 0) {
    if (mqd_ == (mqd_t)-1) return false;
    if (len > static_cast<size_t>(cfg_.msgsize)) {
      throw std::runtime_error("mq_send message too large");
    }
    if (mq_send(mqd_, reinterpret_cast<const char*>(data), len, prio) != 0) {
      if (errno == EAGAIN) return false;
      throw std::runtime_error("mq_send failed: " + std::string(std::strerror(errno)));
    }
    return true;
  }

  // blocking receive unless nonblock configured
  ssize_t recv(uint8_t* buf, size_t cap, unsigned* prio = nullptr) {
    if (mqd_ == (mqd_t)-1) return -1;
    if (cap < static_cast<size_t>(cfg_.msgsize)) {
      throw std::runtime_error("recv buffer too small");
    }
    unsigned p = 0;
    ssize_t n = mq_receive(mqd_, reinterpret_cast<char*>(buf), cap, &p);
    if (n < 0) {
      if (errno == EAGAIN) return -1;
      throw std::runtime_error("mq_receive failed: " + std::string(std::strerror(errno)));
    }
    if (prio) *prio = p;
    return n;
  }

  long msgsize() const { return cfg_.msgsize; }
  const MqConfig& cfg() const { return cfg_; }

private:
  mqd_t mqd_{(mqd_t)-1};
  MqConfig cfg_{};
};

} // namespace tr
