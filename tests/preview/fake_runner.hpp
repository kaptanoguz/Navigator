// tests/preview/fake_runner.hpp: an IProcessRunner that never spawns anything. It records each
// request and "runs" the thumbnailer by writing a fixture PNG to the path after "-o" (or the
// last argument ending in out.png). Thread-safe (mutex).
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

#include <navigator/platform.hpp>

#include "test_util.hpp"

namespace navigator::test {

class FakeRunner final : public platform::IProcessRunner {
public:
    enum class Mode { WritePng, ExitFailure, Timeout, NoOutput, Garbage, Symlink, Block };

    Mode mode = Mode::WritePng;
    std::uint32_t width = 300;
    std::uint32_t height = 200;

    Result<platform::ProcessResult> run(const platform::ProcessRequest& request,
                                        std::stop_token stop) override {
        {
            const std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        const int now = ++running_;
        int prev = max_running_.load();
        while (now > prev && !max_running_.compare_exchange_weak(prev, now)) {
        }
        struct Dec {
            std::atomic<int>& r;
            ~Dec() { --r; }
        } dec{running_};
        platform::ProcessResult res;
        res.exit_code = 0;
        std::filesystem::path out;
        for (const std::string& a : request.argv) {
            if (a.size() >= 7 && a.compare(a.size() - 7, 7, "out.png") == 0) {
                out = a;
            }
        }
        switch (mode) {
        case Mode::WritePng: write_file(out, encode_png(quadrants(width, height))); break;
        case Mode::ExitFailure: res.exit_code = 1; break;
        case Mode::Timeout:
            res.exit_code = -1;
            res.term_signal = 9;
            res.timed_out = true;
            break;
        case Mode::NoOutput: break;
        case Mode::Garbage: write_text(out, "this is not an image"); break;
        case Mode::Symlink: std::filesystem::create_symlink("/etc/hostname", out); break;
        case Mode::Block:
            while (!stop.stop_requested() && !release_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            write_file(out, encode_png(quadrants(width, height)));
            break;
        }
        return res;
    }

    std::vector<platform::ProcessRequest> requests() {
        const std::lock_guard lock(mutex_);
        return requests_;
    }
    int max_running() const { return max_running_.load(); }
    void release() { release_ = true; }

private:
    std::mutex mutex_;
    std::vector<platform::ProcessRequest> requests_;
    std::atomic<int> running_{0};
    std::atomic<int> max_running_{0};
    std::atomic<bool> release_{false};
};

} // namespace navigator::test
