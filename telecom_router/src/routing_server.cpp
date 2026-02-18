#include "common.hpp"
#include "ipc_mq.hpp"
#include "protocol.hpp"
#include "thread_pool.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

using namespace tr;

namespace {

constexpr int MAX_EVENTS = 256;
constexpr size_t MAX_PENDING = 100000; // backpressure
constexpr int ACCEPT_BACKLOG = 512;

struct Pending {
  std::mutex mu;
  std::condition_variable cv;
  bool done{false};
  std::string resp;
};

struct Conn {
  int fd{-1};
  std::string inbuf;
  std::deque<std::string> outq;
  bool want_write{false};
};

int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
  return 0;
}

std::string trim_newline(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

} // namespace

int main(int argc, char** argv) {
  std::string host = "0.0.0.0";
  int port = 5555;
  if (argc >= 2) host = argv[1];
  if (argc >= 3) port = std::atoi(argv[2]);

  const std::string REQ  = "/tr_mq_req";
  const std::string RESP = "/tr_mq_resp";

  PosixMq mq_req, mq_resp;
  // Server expects queues already created (engine creates them).
  mq_req.open(MqConfig{REQ, 2048, 8192, false, true});   // nonblock helps under load
  mq_resp.open(MqConfig{RESP, 2048, 8192, false, true}); // nonblock for dispatcher polling

  log_info("Routing server starting on " + host + ":" + std::to_string(port));

  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) throw std::runtime_error("socket failed");

  int yes = 1;
  (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
  (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    throw std::runtime_error("bad bind address");
  }
  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw std::runtime_error("bind failed: " + std::string(std::strerror(errno)));
  }
  if (set_nonblock(listen_fd) != 0) throw std::runtime_error("listen nonblock failed");
  if (listen(listen_fd, ACCEPT_BACKLOG) != 0) throw std::runtime_error("listen failed");

  int ep = epoll_create1(0);
  if (ep < 0) throw std::runtime_error("epoll_create1 failed");

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd;
  if (epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
    throw std::runtime_error("epoll_ctl add listen failed");
  }

  // Connection table
  std::unordered_map<int, Conn> conns;

  // Pending response map (corr_id -> Pending)
  std::mutex pend_mu;
  std::unordered_map<uint64_t, std::shared_ptr<Pending>> pending;

  // Response dispatcher thread (reads MQ RESP and completes pending)
  std::atomic<bool> run{true};
  std::thread resp_thread([&]{
    std::vector<uint8_t> buf(static_cast<size_t>(mq_resp.msgsize()));
    while (run.load()) {
      ssize_t n = -1;
      try { n = mq_resp.recv(buf.data(), buf.size(), nullptr); }
      catch (const std::exception& e) {
        log_err(std::string("resp mq recv: ") + e.what());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (n < 0) { // EAGAIN
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      MsgHdr h{};
      std::string payload;
      if (!unpack(buf.data(), static_cast<size_t>(n), h, payload)) continue;
      if (static_cast<MsgType>(h.type) != MsgType::RouteResp) continue;

      std::shared_ptr<Pending> p;
      {
        std::lock_guard<std::mutex> lk(pend_mu);
        auto it = pending.find(h.corr_id);
        if (it != pending.end()) { p = it->second; pending.erase(it); }
      }
      if (p) {
        std::lock_guard<std::mutex> lk(p->mu);
        p->resp = payload;
        p->done = true;
        p->cv.notify_one();
      }
    }
  });

  // Worker pool for request processing (MQ send + wait response)
  const size_t nworkers = std::max<size_t>(2, std::thread::hardware_concurrency());
  ThreadPool pool(nworkers);

  auto close_conn = [&](int fd) {
    (void)epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    conns.erase(fd);
  };

  auto enable_write = [&](int fd, bool on) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    epoll_event e{};
    e.data.fd = fd;
    e.events = EPOLLIN | (on ? EPOLLOUT : 0);
    (void)epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e);
    it->second.want_write = on;
  };

  epoll_event events[MAX_EVENTS];

  while (true) {
    int n = epoll_wait(ep, events, MAX_EVENTS, 1000);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("epoll_wait failed");
    }

    for (int i = 0; i < n; ++i) {
      int fd = events[i].data.fd;
      uint32_t ee = events[i].events;

      if (fd == listen_fd) {
        for (;;) {
          sockaddr_in caddr{};
          socklen_t clen = sizeof(caddr);
          int cfd = accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
          if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_warn("accept error");
            break;
          }
          (void)set_nonblock(cfd);
          epoll_event cev{};
          cev.data.fd = cfd;
          cev.events = EPOLLIN;
          (void)epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
          conns.emplace(cfd, Conn{cfd});
        }
        continue;
      }

      auto it = conns.find(fd);
      if (it == conns.end()) continue;

      if (ee & (EPOLLHUP | EPOLLERR)) {
        close_conn(fd);
        continue;
      }

      // Read
      if (ee & EPOLLIN) {
        char buf[2048];
        for (;;) {
          ssize_t r = ::read(fd, buf, sizeof(buf));
          if (r == 0) { close_conn(fd); break; }
          if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(fd);
            break;
          }
          it->second.inbuf.append(buf, buf + r);

          // Line-framed JSON requests
          for (;;) {
            auto pos = it->second.inbuf.find('\n');
            if (pos == std::string::npos) break;
            std::string line = it->second.inbuf.substr(0, pos + 1);
            it->second.inbuf.erase(0, pos + 1);
            line = trim_newline(line);
            if (line.empty()) continue;

            // Backpressure: too many pending transactions
            {
              std::lock_guard<std::mutex> lk(pend_mu);
              if (pending.size() > MAX_PENDING) {
                it->second.outq.emplace_back("{\"status\":\"BUSY\",\"reason\":\"overload\"}\n");
                enable_write(fd, true);
                continue;
              }
            }

            const uint64_t corr = next_corr_id();
            auto pend = std::make_shared<Pending>();
            {
              std::lock_guard<std::mutex> lk(pend_mu);
              pending.emplace(corr, pend);
            }

            pool.submit([&, fd, corr, pend, req=line] {
              try {
                auto msg = pack(MsgType::RouteReq, corr, req);

                // Retry send if MQ is temporarily full
                bool sent = false;
                for (int k = 0; k < 1000 && !sent; ++k) {
                  sent = mq_req.send(msg.data(), msg.size(), 0);
                  if (!sent) std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                if (!sent) {
                  std::lock_guard<std::mutex> lk(pend->mu);
                  pend->resp = "{\"status\":\"ERROR\",\"reason\":\"mq_full\"}";
                  pend->done = true;
                  pend->cv.notify_one();
                }

                // Wait for FLX response (bounded)
                {
                  std::unique_lock<std::mutex> lk(pend->mu);
                  if (!pend->cv.wait_for(lk, std::chrono::milliseconds(500), [&]{ return pend->done; })) {
                    pend->resp = "{\"status\":\"TIMEOUT\",\"reason\":\"flx_no_response\"}";
                    pend->done = true;
                  }
                }

                // Enqueue response for socket write
                auto resp_line = pend->resp + "\n";
                static std::mutex conns_mu; // coarse safety for demo
                std::lock_guard<std::mutex> lk(conns_mu);
                auto it2 = conns.find(fd);
                if (it2 != conns.end()) {
                  it2->second.outq.emplace_back(std::move(resp_line));
                  enable_write(fd, true);
                }
              } catch (const std::exception& e) {
                log_err(std::string("worker req error: ") + e.what());
              }
            });
          }
        }
      }

      // Write
      if (ee & EPOLLOUT) {
        auto &c = it->second;
        while (!c.outq.empty()) {
          const std::string& s = c.outq.front();
          ssize_t w = ::write(fd, s.data(), s.size());
          if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(fd);
            break;
          }
          if (static_cast<size_t>(w) < s.size()) {
            // partial write
            c.outq.front() = s.substr(static_cast<size_t>(w));
            break;
          }
          c.outq.pop_front();
        }
        if (conns.find(fd) != conns.end()) {
          if (c.outq.empty()) enable_write(fd, false);
        }
      }
    }
  }

  run = false;
  resp_thread.join();
  return 0;
}
