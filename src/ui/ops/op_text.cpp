// src/ui/ops/op_text.cpp — see op_text.hpp.
#include "ops/op_text.hpp"

#include <algorithm>
#include <cmath>

#include <navigator/fs.hpp>
#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"

namespace navigator::ui {
namespace {

std::string fill(std::string text, std::string_view name, std::string_view folder,
                 std::string_view label) {
    text = substitute(text, "name", name);
    text = substitute(text, "folder", folder);
    text = substitute(text, "label", label);
    return text;
}

} // namespace

std::string kind_key(ops::OpKind kind) { return std::string(ops::to_string(kind)); }

std::string op_title(ops::OpKind kind, std::size_t count, std::string_view name,
                     std::string_view folder, std::string_view label) {
    return fill(tr_count("ops.title." + kind_key(kind), count), name, folder, label);
}

std::string op_done_text(ops::OpKind kind, std::size_t count, std::string_view name,
                         std::string_view folder, std::string_view label) {
    return fill(tr_count("ops.done." + kind_key(kind), count), name, folder, label);
}

std::string op_partial_text(ops::OpKind kind, std::size_t done, std::size_t total, bool cancelled) {
    std::string text(
        tr(cancelled ? std::string("ops.cancelled_partial") : "ops.partial." + kind_key(kind)));
    text = substitute(text, "done", format_count(done));
    text = substitute(text, "total", format_count(total));
    return text;
}

std::string error_sentence(ErrorCode code, std::string_view item_name) {
    std::string key = "ops.err.";
    key += to_string(code);
    std::string text(tr(key));
    return substitute(text, "name", item_name);
}

std::string changed_sentence(std::string_view item_name) {
    return substitute(std::string(tr("ops.err.changed")), "name", item_name);
}

std::string undo_label_text(const ops::UndoLabel& label) {
    std::string text = tr_count(label.key, label.count);
    text = substitute(text, "name", label.name);
    text = substitute(text, "folder", label.folder);
    text = substitute(text, "count", format_count(label.count));
    return text;
}

std::string format_rate(double bytes_per_second) {
    const auto bytes = static_cast<std::uint64_t>(std::max(0.0, bytes_per_second));
    return substitute(std::string(tr("ops.rate")), "size", format_size(bytes));
}

std::string format_eta(std::chrono::seconds eta) {
    const long long s = std::max<long long>(0, eta.count());
    std::string text;
    if (s >= 3600) {
        text = substitute(std::string(tr("ops.eta.hours")), "h", std::to_string(s / 3600));
        text = substitute(text, "m", std::to_string((s % 3600) / 60));
    } else if (s >= 60) {
        text = substitute(std::string(tr("ops.eta.minutes")), "m", std::to_string(s / 60));
        text = substitute(text, "s", std::to_string(s % 60));
    } else {
        text =
            substitute(std::string(tr("ops.eta.seconds")), "s", std::to_string(std::max(1LL, s)));
    }
    return text;
}

std::string folder_display(const Path& path) {
    if (path.empty()) {
        return {};
    }
    const Path name = path.filename();
    if (name.empty()) {
        return path.root_path().string().empty() ? std::string() : path.root_path().string();
    }
    return fs::to_display_name(name.string());
}

std::optional<double> op_fraction(const ops::OpProgress& p) {
    if (p.phase == ops::OpPhase::Scanning || p.phase == ops::OpPhase::Queued ||
        p.phase == ops::OpPhase::Waiting) {
        return std::nullopt;
    }
    if (p.totals_final && p.bytes_total > 0) {
        return std::clamp(static_cast<double>(p.bytes_done) / static_cast<double>(p.bytes_total),
                          0.0, 1.0);
    }
    if (p.items_total > 0 && p.totals_final) {
        return std::clamp(static_cast<double>(p.items_done) / static_cast<double>(p.items_total),
                          0.0, 1.0);
    }
    if (p.top_level_total > 0) {
        return std::clamp(static_cast<double>(p.top_level_done) /
                              static_cast<double>(p.top_level_total),
                          0.0, 1.0);
    }
    return std::nullopt;
}

} // namespace navigator::ui
