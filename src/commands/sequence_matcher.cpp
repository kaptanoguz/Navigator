// SequenceMatcher: the state machine behind multi-chord key bindings (docs/keymap.md "Key
// sequences"). UI-thread only; it never holds a reference to the Keymap.
#include <algorithm>
#include <chrono>
#include <optional>
#include <vector>

#include <navigator/commands.hpp>

namespace navigator {
namespace {

std::chrono::milliseconds clamp_timeout(std::chrono::milliseconds t) noexcept {
    return std::clamp(t, kMinSequenceTimeout, kMaxSequenceTimeout);
}

} // namespace

SequenceMatcher::SequenceMatcher(std::chrono::milliseconds timeout) noexcept
    : timeout_(clamp_timeout(timeout)) {}

void SequenceMatcher::set_timeout(std::chrono::milliseconds timeout) noexcept {
    timeout_ = clamp_timeout(timeout);
}

std::chrono::milliseconds SequenceMatcher::timeout() const noexcept { return timeout_; }

void SequenceMatcher::reset() noexcept { pending_.clear(); }

std::optional<SequenceMatcher::Clock::time_point> SequenceMatcher::deadline() const noexcept {
    if (pending_.empty()) {
        return std::nullopt;
    }
    return last_ + std::chrono::duration_cast<Clock::duration>(timeout_);
}

bool SequenceMatcher::expire(Clock::time_point now) noexcept {
    if (pending_.empty()) {
        return false;
    }
    // Times that go backwards count as zero elapsed; the comparison is strictly greater.
    if (now > last_ && now - last_ > timeout_) {
        pending_.clear();
        return true;
    }
    return false;
}

SequenceResult SequenceMatcher::feed(const Keymap& keymap, KeyChord chord, Clock::time_point now) {
    SequenceResult result;
    result.expired = expire(now);

    if (!pending_.empty()) {
        if (chord.key == Key::Escape && chord.mods == Modifiers::None) {
            result.kind = SequenceResult::Kind::Cancelled;
            result.consumed = true;
            result.sequence = std::move(pending_);
            pending_.clear();
            return result;
        }
        std::vector<KeyChord> candidate = pending_;
        candidate.push_back(chord);
        const Keymap::SequenceLookup found = keymap.lookup(candidate);
        result.consumed = true;
        if (found.exact.has_value()) {
            result.kind = SequenceResult::Kind::Match;
            result.binding = found.exact;
            result.sequence = std::move(candidate);
            pending_.clear();
        } else if (found.is_prefix) {
            result.kind = SequenceResult::Kind::Pending;
            pending_ = candidate;
            last_ = now;
            result.sequence = std::move(candidate);
        } else {
            result.kind = SequenceResult::Kind::Aborted;
            result.sequence = std::move(candidate);
            pending_.clear();
        }
        return result;
    }

    const KeyChord first[1] = {chord};
    const Keymap::SequenceLookup found = keymap.lookup(first);
    if (found.is_prefix) {
        result.kind = SequenceResult::Kind::Pending;
        result.consumed = true;
        pending_.push_back(chord);
        last_ = now;
        result.sequence = pending_;
    }
    return result;
}

} // namespace navigator
