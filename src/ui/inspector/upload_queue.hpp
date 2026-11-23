// src/ui/inspector/upload_queue.hpp
//
// Phase 6: GPU uploads of finished preview pixels are spread over frames (docs/architecture.md
// "Phase 6", data flow): at frame start the ui uploads at most a byte budget, the rest waits for
// the next frame, so a burst of thumbnails or one huge image never stalls a frame.
//
// UploadQueue<T> is the pure scheduler: jobs carry an id (deduplicated), a class (Inspector before
// thumbnails, FIFO within a class) and a byte size; a job may be split into bands (rows) so a
// 128 MiB 1:1 image uploads over several frames. plan() returns this frame's slices: whole jobs
// and bands while the budget lasts, and always at least one band when anything is queued (so
// progress is guaranteed even when one band exceeds the budget).
// Thread-safety: UI thread only (plain container).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace navigator::ui::inspector {

inline constexpr std::size_t kUploadBudgetBytes = std::size_t{8} * 1024U * 1024U;

enum class UploadClass : std::uint8_t {
    Inspector = 0, // the inspected item: first
    Thumbnail = 1,
};

template <typename T> class UploadQueue {
public:
    struct Job {
        std::uint64_t id = 0;
        UploadClass cls = UploadClass::Thumbnail;
        std::uint32_t rows = 1;     // bands of one row each
        std::size_t row_bytes = 0;  // bytes per row
        std::uint32_t next_row = 0; // rows already uploaded
        T payload{};
    };
    struct Slice {
        std::uint64_t id = 0;
        std::uint32_t first_row = 0;
        std::uint32_t row_count = 0;
        bool last = false; // the job is complete after this slice
        T payload{};
    };

    // Queues a job; an id already queued is replaced (its progress restarts).
    void push(std::uint64_t id, UploadClass cls, std::uint32_t rows, std::size_t row_bytes,
              T payload) {
        remove(id);
        Job j;
        j.id = id;
        j.cls = cls;
        j.rows = std::max<std::uint32_t>(rows, 1U);
        j.row_bytes = row_bytes;
        j.payload = std::move(payload);
        jobs_.push_back(std::move(j));
    }

    void remove(std::uint64_t id) {
        jobs_.erase(
            std::remove_if(jobs_.begin(), jobs_.end(), [id](const Job& j) { return j.id == id; }),
            jobs_.end());
    }

    [[nodiscard]] bool contains(std::uint64_t id) const noexcept {
        return std::any_of(jobs_.begin(), jobs_.end(), [id](const Job& j) { return j.id == id; });
    }
    [[nodiscard]] bool empty() const noexcept { return jobs_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return jobs_.size(); }
    // Bytes still to upload.
    [[nodiscard]] std::size_t pending_bytes() const noexcept {
        std::size_t n = 0;
        for (const Job& j : jobs_) {
            n += static_cast<std::size_t>(j.rows - j.next_row) * j.row_bytes;
        }
        return n;
    }

    // This frame's slices within `budget` bytes; completed jobs leave the queue.
    [[nodiscard]] std::vector<Slice> plan(std::size_t budget) {
        std::vector<Slice> out;
        std::stable_sort(jobs_.begin(), jobs_.end(), [](const Job& a, const Job& b) {
            return static_cast<int>(a.cls) < static_cast<int>(b.cls);
        });
        std::size_t used = 0;
        for (Job& j : jobs_) {
            if (used >= budget && !out.empty()) {
                break;
            }
            const std::uint32_t left = j.rows - j.next_row;
            std::uint32_t take = left;
            if (j.row_bytes > 0) {
                const std::size_t room = budget > used ? budget - used : 0;
                const std::size_t fit = room / j.row_bytes;
                take = static_cast<std::uint32_t>(std::min<std::size_t>(left, fit));
                if (take == 0) {
                    if (!out.empty()) {
                        break;
                    }
                    take = 1; // progress guarantee
                }
            }
            Slice s;
            s.id = j.id;
            s.first_row = j.next_row;
            s.row_count = take;
            j.next_row += take;
            s.last = j.next_row >= j.rows;
            s.payload = j.payload;
            used += static_cast<std::size_t>(take) * j.row_bytes;
            out.push_back(std::move(s));
            if (!out.back().last) {
                break; // a partially uploaded job holds the budget
            }
        }
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                   [](const Job& j) { return j.next_row >= j.rows; }),
                    jobs_.end());
        return out;
    }

private:
    std::deque<Job> jobs_;
};

} // namespace navigator::ui::inspector
