// src/ui/search/index_status.cpp — see index_status.hpp.
#include "search/index_status.hpp"

#include <algorithm>
#include <cmath>

#include <navigator/i18n.hpp>

#include "i18n/catalog.hpp"

namespace navigator::ui {

std::optional<float> index_scan_fraction(const search::IndexStatus& status) noexcept {
    // Nothing pending while Scanning means "not counted yet" (the first snapshot), not "done".
    if (status.directories == 0 || status.dirs_pending == 0) {
        return std::nullopt;
    }
    const std::uint64_t pending = std::min(status.dirs_pending, status.directories);
    const auto done = static_cast<double>(status.directories - pending);
    return static_cast<float>(
        std::clamp(done / static_cast<double>(status.directories), 0.0, 0.99));
}

IndexStatusView present_index_status(const search::IndexStatus& status) {
    IndexStatusView v;
    const std::string items = tr_count("index.items", status.entries);
    switch (status.state) {
    case search::IndexState::Idle:
        v.text = std::string(tr("index.idle"));
        v.tone = IndexTone::Quiet;
        break;
    case search::IndexState::Loading:
        v.text = std::string(tr("index.loading"));
        v.tone = IndexTone::Busy;
        break;
    case search::IndexState::Scanning: {
        v.fraction = index_scan_fraction(status);
        v.tone = IndexTone::Busy;
        if (v.fraction) {
            const auto pct = static_cast<std::uint64_t>(std::floor(*v.fraction * 100.0F));
            v.text = substitute(substitute(tr("index.scanning_pct"), "pct", std::to_string(pct)),
                                "items", items);
        } else {
            v.text = substitute(tr("index.scanning"), "items", items);
        }
        break;
    }
    case search::IndexState::Verifying:
        v.text = substitute(tr("index.verifying"), "items", items);
        v.tone = IndexTone::Ready;
        break;
    case search::IndexState::Ready:
        v.text = substitute(tr(status.truncated ? "index.ready_truncated" : "index.ready"), "items",
                            items);
        v.tone = IndexTone::Ready;
        break;
    case search::IndexState::Failed:
        v.text = std::string(tr("index.failed"));
        v.tone = IndexTone::Problem;
        break;
    case search::IndexState::Stopped:
        v.text = std::string(tr("index.stopped"));
        v.tone = IndexTone::Quiet;
        break;
    }
    return v;
}

} // namespace navigator::ui
