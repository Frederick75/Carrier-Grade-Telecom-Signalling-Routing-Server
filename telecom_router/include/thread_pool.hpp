#pragma once
#include "common.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

namespace tr {

class ThreadPool {
public:
  explicit ThreadPool(size_t n) : stop_(false) {
    if (n == 0) n = 1;
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }

  void submit(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (stop_) throw std::runtime_error("ThreadPool stopped");
      q_.push(std::move(fn));
    }
    cv_.notify_one();
  }

private:
  void worker_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&]{ return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) return;
        job = std::move(q_.front());
        q_.pop();
      }
      try { job(); }
      catch (const std::exception& e) { log_err(std::string("worker exception: ") + e.what()); }
      catch (...) { log_err("worker unknown exception"); }
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> q_;
  std::vector<std::thread> workers_;
  bool stop_;
};

} // namespace tr
