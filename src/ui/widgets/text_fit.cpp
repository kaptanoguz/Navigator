// src/ui/widgets/text_fit.cpp — see text_fit.hpp.
#include "widgets/text_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace navigator::ui {
namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // U+2026

struct Boundary {
    std::size_t byte = 0; // offset into the text where a cluster starts
    float x = 0.0F;       // pen position at that cluster
};

// Cluster starts in logical order, plus the end of the text.
std::vector<Boundary> boundaries(const text::TextLayout& layout, std::size_t size) {
    std::vector<Boundary> out;
    for (const text::TextLayout::Run& run : layout.runs) {
        for (const text::ShapedGlyph& g : run.shaped.glyphs) {
            const float x = run.x + g.x;
            if (!out.empty() && out.back().byte == g.cluster) {
                out.back().x = std::min(out.back().x, x);
                continue;
            }
            out.push_back(Boundary{g.cluster, x});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Boundary& a, const Boundary& b) { return a.byte < b.byte; });
    out.push_back(Boundary{size, layout.advance});
    return out;
}

std::string_view trim_right(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view trim_left(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    return s;
}

} // namespace

std::string TextFitter::elide(const Painter& painter, const TextStyle& style, std::string_view text,
                              float max_px, Elide mode) {
    const ShapedText full = painter.shape(style, text);
    if (!full.layout || full.width() <= max_px) {
        return std::string(text);
    }
    const float ell = painter.shape(style, kEllipsis).width();
    const std::vector<Boundary> b = boundaries(*full.layout, text.size());
    const float total = full.width();

    // Index of the last boundary whose x fits `limit` (0 if none).
    const auto head_index = [&](float limit, std::size_t upto) {
        std::size_t best = 0;
        for (std::size_t i = 0; i < b.size() && b[i].byte <= upto; ++i) {
            if (b[i].x <= limit) {
                best = i;
            }
        }
        return best;
    };

    std::string candidate;
    if (mode == Elide::Middle && text.size() > 4) {
        // Tail: up to 40% of the width, extended to keep the whole extension when it is short.
        std::size_t tail = b.size() - 1;
        for (std::size_t i = b.size() - 1; i-- > 0;) {
            if (total - b[i].x <= max_px * 0.4F) {
                tail = i;
            } else {
                break;
            }
        }
        const std::size_t dot = text.rfind('.');
        if (dot != std::string_view::npos && dot > 0 && text.size() - dot <= 12) {
            for (std::size_t i = 0; i < b.size(); ++i) {
                if (b[i].byte >= dot) {
                    if (i < tail && total - b[i].x <= max_px * 0.55F) {
                        tail = i;
                    }
                    break;
                }
            }
        }
        const float tail_w = total - b[tail].x;
        std::size_t head = head_index(max_px - ell - tail_w, b[tail].byte);
        for (int tries = 0; head > 0 && tries < 6; ++tries) {
            candidate.assign(trim_right(text.substr(0, b[head].byte)));
            candidate += kEllipsis;
            candidate += trim_left(text.substr(b[tail].byte));
            if (painter.shape(style, candidate).width() <= max_px) {
                return candidate;
            }
            --head;
        }
        // Too narrow for head + tail: fall back to end elision.
    }
    std::size_t head = head_index(max_px - ell, text.size());
    for (int tries = 0;; ++tries) {
        candidate.assign(trim_right(text.substr(0, b[head].byte)));
        candidate += kEllipsis;
        if (head == 0 || painter.shape(style, candidate).width() <= max_px) {
            break;
        }
        head = tries < 6 ? head - 1 : head / 2;
    }
    return candidate;
}

ShapedText TextFitter::fit(const Painter& painter, const TextStyle& style, std::string_view text,
                           float max_px, Elide mode) {
    return fit_text(painter, style, text, max_px, mode).shaped;
}

TextFitter::Fitted TextFitter::fit_text(const Painter& painter, const TextStyle& style,
                                        std::string_view text, float max_px, Elide mode) {
    ShapedText full = painter.shape(style, text);
    if (!full.layout || full.width() <= max_px) {
        return Fitted{std::move(full), {}};
    }
    key_.clear();
    key_ += std::to_string(static_cast<int>(style.size * 8.0F));
    key_ += ':';
    key_ += std::to_string(static_cast<int>(style.weight));
    key_ += style.tabular ? 't' : 'p';
    key_ += mode == Elide::Middle ? 'm' : 'e';
    key_ += std::to_string(static_cast<int>(std::floor(max_px)));
    key_ += ':';
    key_ += std::to_string(static_cast<int>(painter.scale() * 100.0F));
    key_ += '|';
    key_ += text;
    if (const auto hit = cache_.find(key_)) {
        return *hit;
    }
    std::string shown = elide(painter, style, text, max_px, mode);
    ShapedText shaped = painter.shape(style, shown);
    return *cache_.insert(key_, Fitted{std::move(shaped), std::move(shown)});
}

} // namespace navigator::ui
