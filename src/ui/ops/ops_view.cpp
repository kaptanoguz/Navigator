// src/ui/ops/ops_view.cpp — see ops_view.hpp.
#include "ops/ops_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "ops/op_text.hpp"
#include "widgets/icons.hpp"
#include "widgets/text_field.hpp"

namespace navigator::ui {
namespace {

Color faded(const Color& c, float a) { return c.with_alpha(c.a * a); }

float hairline(const Painter& p) { return std::max(1.0F, std::round(p.px(1.0F))); }

// Greedy word wrap of `text` into lines of at most `max_px`.
std::vector<std::string> wrap_lines(const Painter& p, const TextStyle& style, std::string_view text,
                                    float max_px) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t sp = text.find(' ', pos);
        const std::string word(
            text.substr(pos, sp == std::string_view::npos ? std::string_view::npos : sp - pos));
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && p.shape(style, candidate).width() > max_px) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        if (sp == std::string_view::npos) {
            break;
        }
        pos = sp + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

fs::DirEntry entry_of_side(const ops::ConflictSide& s) {
    fs::DirEntry e;
    e.name = s.path.filename().string();
    e.kind = s.kind;
    e.size = s.size;
    e.mtime = s.mtime;
    e.mode = s.mode;
    e.readable = true;
    e.hidden = fs::is_hidden_name(e.name);
    return e;
}

std::string_view button_label_key(ConflictButton b, const ops::Conflict& c) {
    switch (b) {
    case ConflictButton::Replace:
        return c.replace_is_permanent ? "conflict.replace_permanent" : "conflict.replace_trash";
    case ConflictButton::Skip: return "conflict.skip";
    case ConflictButton::KeepBoth: return "conflict.keep_both";
    case ConflictButton::Merge: return "conflict.merge";
    case ConflictButton::Cancel: return "conflict.cancel";
    }
    return "conflict.skip";
}

} // namespace

bool OpsView::hit(float x, float y) const {
    return std::ranges::any_of(hits_, [&](const RectF& r) { return contains(r, x, y); });
}

bool OpsView::button(const OpsFrame& f, const RectF& r, std::size_t id, bool /*focused_ring*/) {
    const PointerInput& ptr = f.pointer;
    const bool inside = contains(r, ptr.x, ptr.y);
    if (inside && ptr.pressed) {
        pressed_ = id;
    }
    return pressed_ == id && ptr.released && inside;
}

RectF OpsView::card(const OpsFrame& f, const RectF& rect, float radius_dp, bool scrim) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    if (scrim) {
        r.fill_rect(RectF{0.0F, 0.0F, f.width, f.height}, f.theme == Theme::Dark
                                                              ? Color{0.0F, 0.0F, 0.01F, 0.42F}
                                                              : faded(pal.shadow, 0.9F));
    }
    const RectF box = snap(rect);
    const float rad = std::round(p.px(radius_dp));
    r.drop_shadow(box, rad, p.px(scrim ? 36.0F : 26.0F), p.px(scrim ? 10.0F : 8.0F), pal.shadow);
    r.fill_rounded_rect(box, rad, pal.surface_raised);
    r.stroke_rounded_rect(box, rad, hairline(p), pal.border);
    next_hits_.push_back(box);
    return box;
}

void OpsView::progress_bar(const OpsFrame& f, const RectF& rect, std::optional<double> fraction,
                           bool paused) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const RectF track = snap(rect);
    const float rad = 0.5F * track.h;
    r.fill_rounded_rect(track, rad,
                        pal.control_hover.with_alpha(std::max(0.5F, pal.control_hover.a)));
    const Color fill = paused ? pal.text_muted : pal.accent;
    if (fraction) {
        const float w = std::max(track.h, track.w * static_cast<float>(*fraction));
        r.fill_rounded_rect(RectF{track.x, track.y, std::round(w), track.h}, rad, fill);
    } else {
        const float seg = 0.32F * track.w;
        const double t = std::fmod(f.now_ms / 1100.0, 1.0);
        const float x = track.x - seg + (static_cast<float>(t) * (track.w + seg));
        const float x0 = std::max(x, track.x);
        const float x1 = std::min(x + seg, track.right());
        if (x1 > x0) {
            r.fill_rounded_rect(RectF{std::round(x0), track.y, std::round(x1 - x0), track.h}, rad,
                                fill);
        }
        animating_ = true;
    }
}

void OpsView::op_glyph(const OpsFrame& f, ops::OpKind kind, float cx, float cy, float size,
                       const Color& color) {
    Renderer& r = f.painter.renderer();
    const float u = size / 16.0F;
    const float t = std::max(1.0F, 1.4F * u);
    const auto line = [&](float x0, float y0, float x1, float y1) {
        r.stroke_line(cx + (x0 * u), cy + (y0 * u), cx + (x1 * u), cy + (y1 * u), t, color);
    };
    const auto box = [&](float x, float y, float w, float h) {
        r.stroke_rounded_rect(RectF{cx + (x * u), cy + (y * u), w * u, h * u}, 2.0F * u, t, color);
    };
    switch (kind) {
    case ops::OpKind::Copy:
        box(-6.5F, -6.5F, 9.5F, 9.5F);
        box(-3.0F, -3.0F, 9.5F, 9.5F);
        break;
    case ops::OpKind::Move:
    case ops::OpKind::Redo:
        line(-7, 0, 6, 0);
        line(1, -5, 6, 0);
        line(1, 5, 6, 0);
        break;
    case ops::OpKind::Undo:
        line(7, 0, -6, 0);
        line(-1, -5, -6, 0);
        line(-1, 5, -6, 0);
        break;
    case ops::OpKind::Trash:
    case ops::OpKind::DeletePermanently:
        line(-6, -4, 6, -4);
        line(-2, -6.5F, 2, -6.5F);
        box(-4.5F, -3.0F, 9.0F, 9.5F);
        if (kind == ops::OpKind::DeletePermanently) {
            line(-1.5F, 0, 1.5F, 3.5F);
            line(1.5F, 0, -1.5F, 3.5F);
        }
        break;
    case ops::OpKind::Restore:
        line(-5, 5, 5, -5);
        line(-5, 5, -5, -1);
        line(-5, 5, 1, 5);
        break;
    case ops::OpKind::Rename:
        line(-6, 6, 4, -4);
        line(2, -6, 6, -2);
        line(-6, 6, -6, 3);
        line(-6, 6, -3, 6);
        break;
    case ops::OpKind::MakeDirectory:
        box(-7.0F, -4.5F, 14.0F, 10.0F);
        line(-7, -2, 0, -2);
        line(-1, 0.5F, -1, 4.0F);
        line(-2.75F, 2.25F, 0.75F, 2.25F);
        break;
    case ops::OpKind::CreateSymlink:
        r.stroke_rounded_rect(RectF{cx - (7.0F * u), cy - (3.0F * u), 8.0F * u, 6.0F * u}, 3.0F * u,
                              t, color);
        r.stroke_rounded_rect(RectF{cx - (1.0F * u), cy - (3.0F * u), 8.0F * u, 6.0F * u}, 3.0F * u,
                              t, color);
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Chip
// ---------------------------------------------------------------------------------------------

namespace {

std::string chip_text(const FileOps& ops) {
    const std::vector<ops::OpProgress>& run = ops.running();
    if (run.empty()) {
        return {};
    }
    for (const ops::OpProgress& p : run) {
        if (p.phase == ops::OpPhase::Waiting) {
            return std::string(tr("ops.state.waiting"));
        }
    }
    if (run.size() > 1) {
        return tr_count("ops.chip.many", run.size());
    }
    std::string text = ops.title_of(run.front());
    if (const std::optional<double> fr = op_fraction(run.front()); fr) {
        text += "  ";
        text += std::to_string(static_cast<int>(std::lround(*fr * 100.0)));
        text += "%";
    }
    return text;
}

} // namespace

float OpsView::chip_width(const OpsFrame& f) {
    if (f.ops.running().empty()) {
        return 0.0F;
    }
    const Painter& p = f.painter;
    const ShapedText t = p.shape(type::status, chip_text(f.ops));
    return std::round(std::min(t.width(), p.px(260.0F)) + p.px(8.0F + 34.0F + 7.0F + 10.0F));
}

void OpsView::draw_chip(const OpsFrame& f, const RectF& slot) {
    const std::vector<ops::OpProgress>& run = f.ops.running();
    if (run.empty() || slot.w <= 0.0F) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float h = std::round(p.px(22.0F));
    const RectF chip = snap(RectF{slot.x, std::round(slot.center_y() - (0.5F * h)), slot.w, h});
    const bool hover = contains(chip, f.pointer.x, f.pointer.y);
    const float rad = 0.5F * h;
    r.fill_rounded_rect(chip, rad, pal.chip);
    if (hover) {
        r.fill_rounded_rect(chip, rad, pal.control_hover);
    }
    r.stroke_rounded_rect(chip, rad, hairline(p), pal.accent.with_alpha(0.35F));
    bool waiting = false;
    bool paused = true;
    std::optional<double> fraction;
    if (run.size() == 1) {
        fraction = op_fraction(run.front());
    }
    for (const ops::OpProgress& op : run) {
        waiting = waiting || op.phase == ops::OpPhase::Waiting;
        paused = paused && op.phase == ops::OpPhase::Paused;
    }
    const float pad = std::round(p.px(8.0F));
    const RectF bar{chip.x + pad, chip.center_y() - std::round(p.px(1.5F)), std::round(p.px(34.0F)),
                    std::round(p.px(3.0F))};
    progress_bar(f, bar, waiting ? std::nullopt : fraction, paused);
    const float tx = bar.right() + std::round(p.px(7.0F));
    const ShapedText t =
        fitter_.fit(p, type::status, chip_text(f.ops), chip.right() - pad - tx, Elide::End);
    p.draw(t, tx, std::round(chip.center_y() + (0.5F * t.cap_height())), pal.chip_text);
    next_hits_.push_back(chip);
    if (button(f, chip, 1)) {
        f.queue.enqueue(CommandId::ToggleOperationsPanel);
    }
}

// ---------------------------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------------------------

void OpsView::draw_panel(const OpsFrame& f) {
    if (!f.ops.panel_open()) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const DensityMetrics dm = density_metrics(f.density);
    const std::vector<ops::OpProgress>& run = f.ops.running();
    const std::deque<FinishedOp>& hist = f.ops.history();
    const float w = std::min(std::round(p.px(440.0F)), f.width - std::round(p.px(24.0F)));
    const float pad = std::round(p.px(14.0F));
    const float head_h = std::round(p.px(40.0F));
    const float run_h = std::round(p.px(86.0F));
    const float fin_h = std::round(p.px(40.0F));
    const float line_h = std::round(p.px(17.0F));

    // Content height.
    float content = 0.0F;
    for (std::size_t i = 0; i < run.size(); ++i) {
        content += run_h;
    }
    float hist_h = 0.0F;
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        hist_h +=
            fin_h + (static_cast<float>(std::min<std::size_t>(it->details.size(), 3)) * line_h);
    }
    content += hist_h;
    if (run.empty() && hist.empty()) {
        content = std::round(p.px(56.0F));
    }
    const float max_h = std::min(f.height * 0.7F, std::round(p.px(520.0F)));
    const float h = std::min(head_h + content + std::round(p.px(8.0F)), max_h);
    const float bottom = f.height - std::round(p.px(dm.status_bar_height + 8.0F));
    const float x = std::min(f.content.right(), f.width) - w - std::round(p.px(12.0F));
    const RectF box =
        card(f, RectF{std::max(x, std::round(p.px(8.0F))), bottom - h, w, h}, 12.0F, false);

    const ShapedText title = p.shape(type::popup_title, tr("ops.panel.title"));
    p.draw(title, box.x + pad, std::round(box.y + (0.5F * head_h) + (0.5F * title.cap_height())),
           pal.text_primary);
    {
        const float s = std::round(p.px(24.0F));
        const RectF close = snap(RectF{box.right() - pad - s + p.px(4.0F),
                                       box.y + std::round(0.5F * (head_h - s)), s, s});
        const bool hv = contains(close, f.pointer.x, f.pointer.y);
        if (hv) {
            r.fill_rounded_rect(close, p.px(radius::sm), pal.control_hover);
        }
        draw_cross(r, close.center_x(), close.center_y(), p.px(4.5F), std::max(1.0F, p.px(1.4F)),
                   hv ? pal.text_primary : pal.text_secondary);
        if (button(f, close, 2)) {
            f.queue.enqueue(CommandId::ToggleOperationsPanel);
        }
    }
    r.fill_rect(RectF{box.x + hairline(p), box.y + head_h - hairline(p),
                      box.w - (2.0F * hairline(p)), hairline(p)},
                pal.border);

    const RectF body{box.x, box.y + head_h, box.w, box.h - head_h};
    // Wheel scroll.
    const float overflow = std::max(0.0F, content - (body.h - std::round(p.px(8.0F))));
    if (f.pointer.wheel_y != 0.0F && contains(box, f.pointer.x, f.pointer.y)) {
        panel_scroll_ -= f.pointer.wheel_y * p.px(48.0F);
    }
    panel_scroll_ = std::clamp(panel_scroll_, 0.0F, overflow);
    r.set_clip(body);
    float y = body.y - panel_scroll_;
    if (run.empty() && hist.empty()) {
        const ShapedText e = p.shape(type::body, tr("ops.panel.empty"));
        p.draw(e, std::round(box.center_x() - (0.5F * e.width())), std::round(y + p.px(32.0F)),
               pal.text_muted);
    }
    std::size_t id = 100;
    for (const ops::OpProgress& op : run) {
        const std::string name = f.ops.title_of(op);
        const float cy = y + std::round(p.px(18.0F));
        op_glyph(f, op.kind, box.x + pad + p.px(8.0F), cy, p.px(15.0F), pal.accent);
        const float bx = box.right() - pad;
        const float s = std::round(p.px(24.0F));
        const RectF cancel = snap(RectF{bx - s, cy - (0.5F * s), s, s});
        const RectF pause = snap(RectF{cancel.x - s - p.px(2.0F), cy - (0.5F * s), s, s});
        const float tx = box.x + pad + std::round(p.px(24.0F));
        const ShapedText t =
            fitter_.fit(p, type::notice_title, name, pause.x - tx - p.px(6.0F), Elide::End);
        p.draw(t, tx, std::round(cy + (0.5F * t.cap_height())), pal.text_primary);
        const bool paused = op.phase == ops::OpPhase::Paused;
        for (int b = 0; b < 2; ++b) {
            const RectF& rr = b == 0 ? pause : cancel;
            const bool hv = contains(rr, f.pointer.x, f.pointer.y);
            if (hv) {
                r.fill_rounded_rect(rr, p.px(radius::sm), pal.control_hover);
            }
            const Color ink = hv ? pal.text_primary : pal.text_secondary;
            if (b == 0) {
                if (paused) {
                    r.fill_polygon({{rr.center_x() - p.px(3.0F), rr.center_y() - p.px(5.0F)},
                                    {rr.center_x() - p.px(3.0F), rr.center_y() + p.px(5.0F)},
                                    {rr.center_x() + p.px(5.0F), rr.center_y()}},
                                   ink);
                } else {
                    const float bw = std::max(2.0F, std::round(p.px(2.5F)));
                    r.fill_rounded_rect(RectF{rr.center_x() - p.px(4.0F),
                                              rr.center_y() - p.px(5.0F), bw, p.px(10.0F)},
                                        0.5F * bw, ink);
                    r.fill_rounded_rect(RectF{rr.center_x() + p.px(1.5F),
                                              rr.center_y() - p.px(5.0F), bw, p.px(10.0F)},
                                        0.5F * bw, ink);
                }
                if (button(f, rr, id)) {
                    f.queue.enqueue(CommandId::TogglePauseOperation, op.id.value);
                }
            } else {
                draw_cross(r, rr.center_x(), rr.center_y(), p.px(4.5F), std::max(1.0F, p.px(1.4F)),
                           ink);
                if (button(f, rr, id + 1)) {
                    f.queue.enqueue(CommandId::CancelOperation, op.id.value);
                }
            }
            id += 2;
        }
        const RectF bar{box.x + pad, y + std::round(p.px(34.0F)), box.w - (2.0F * pad),
                        std::round(p.px(4.0F))};
        progress_bar(f, bar, op_fraction(op), paused);
        // Counts and rate.
        std::string left;
        switch (op.phase) {
        case ops::OpPhase::Scanning: left = std::string(tr("ops.state.scanning")); break;
        case ops::OpPhase::Queued: left = std::string(tr("ops.state.queued")); break;
        case ops::OpPhase::Waiting: left = std::string(tr("ops.state.waiting")); break;
        case ops::OpPhase::Paused: left = std::string(tr("ops.state.paused")); break;
        case ops::OpPhase::Finishing: left = std::string(tr("ops.state.finishing")); break;
        case ops::OpPhase::Working:
        case ops::OpPhase::Done:
            if (op.items_total > 1 || op.top_level_total > 1) {
                std::string a = substitute(std::string(tr("ops.items_progress")), "done",
                                           format_count(op.items_done));
                left =
                    substitute(a, "total", format_count(std::max(op.items_total, op.items_done)));
            }
            break;
        }
        if (op.bytes_total > 0 &&
            (op.phase == ops::OpPhase::Working || op.phase == ops::OpPhase::Paused)) {
            std::string b = substitute(std::string(tr("ops.bytes_progress")), "done",
                                       format_size(op.bytes_done));
            b = substitute(b, "total", format_size(op.bytes_total));
            left = left.empty() ? b : left + "  \xC2\xB7  " + b;
        }
        const float ly = std::round(y + p.px(54.0F));
        std::string right;
        if (op.bytes_per_second > 0.0) {
            right = format_rate(op.bytes_per_second);
            if (op.eta) {
                right += "  \xC2\xB7  " + format_eta(*op.eta);
            }
        }
        const ShapedText rt = p.shape(type::status, right);
        const float lw = box.w - (2.0F * pad) - (right.empty() ? 0.0F : rt.width() + p.px(12.0F));
        const ShapedText lt = fitter_.fit(p, type::status, left, lw, Elide::End);
        p.draw(lt, box.x + pad, ly, pal.text_secondary);
        if (!right.empty()) {
            p.draw(rt, box.right() - pad - rt.width(), ly, pal.text_secondary);
        }
        if (!op.current.empty() && op.phase == ops::OpPhase::Working) {
            const ShapedText ct =
                fitter_.fit(p, type::caption, fs::to_display_name(op.current.filename().string()),
                            box.w - (2.0F * pad), Elide::Middle);
            p.draw(ct, box.x + pad, std::round(y + p.px(72.0F)), pal.text_muted);
        }
        y += run_h;
    }
    // History: newest first.
    bool latest_marked = false;
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        const FinishedOp& h2 = *it;
        const float cy = y + std::round(p.px(20.0F));
        const Color dot = h2.ok ? pal.accent : pal.danger;
        r.fill_circle(box.x + pad + p.px(8.0F), cy, p.px(7.0F), dot.with_alpha(0.16F));
        if (h2.ok) {
            const float cx = box.x + pad + p.px(8.0F);
            r.stroke_line(cx - p.px(3.0F), cy, cx - p.px(0.8F), cy + p.px(2.4F),
                          std::max(1.0F, p.px(1.4F)), dot);
            r.stroke_line(cx - p.px(0.8F), cy + p.px(2.4F), cx + p.px(3.2F), cy - p.px(2.6F),
                          std::max(1.0F, p.px(1.4F)), dot);
        } else {
            draw_alert_glyph(r, box.x + pad + p.px(8.0F), cy, p.px(6.5F), pal.danger,
                             pal.surface_raised, p.scale());
        }
        // Action buttons from the right: Show in folder, Undo / Redo (newest undoable only).
        float rx = box.right() - pad;
        const bool newest_undoable = !latest_marked && h2.undoable;
        if (h2.undoable) {
            latest_marked = true;
        }
        const auto text_button = [&](std::string_view label, bool accent, std::size_t bid,
                                     const std::function<void()>& fire) {
            const ShapedText lt = p.shape(type::button, label);
            const RectF rr = snap(RectF{rx - lt.width() - p.px(14.0F), cy - p.px(11.0F),
                                        lt.width() + p.px(14.0F), p.px(22.0F)});
            const bool hv = contains(rr, f.pointer.x, f.pointer.y);
            if (hv) {
                r.fill_rounded_rect(rr, p.px(radius::sm), pal.control_hover);
            }
            p.draw(lt, rr.x + p.px(7.0F), std::round(cy + (0.5F * lt.cap_height())),
                   accent ? pal.accent : pal.text_secondary);
            if (button(f, rr, bid)) {
                fire();
            }
            rx = rr.x - p.px(2.0F);
        };
        if (newest_undoable && h2.kind != ops::OpKind::DeletePermanently) {
            const bool is_undo = h2.kind == ops::OpKind::Undo;
            if ((is_undo && f.ops.undo_stack().can_redo()) ||
                (!is_undo && f.ops.undo_stack().can_undo())) {
                text_button(tr(is_undo ? "ops.redo" : "ops.undo"), true, id++,
                            [&] { f.queue.enqueue(is_undo ? CommandId::Redo : CommandId::Undo); });
            }
        }
        if (!h2.folder.empty() &&
            (h2.kind == ops::OpKind::Copy || h2.kind == ops::OpKind::Move ||
             h2.kind == ops::OpKind::Restore || h2.kind == ops::OpKind::CreateSymlink)) {
            const Path folder = h2.folder;
            text_button(tr("ops.show_in_folder"), false, id++, [&] {
                Intent in;
                in.kind = Intent::Kind::OpenPath;
                in.path = folder;
                f.intents.push_back(in);
            });
        }
        const float tx = box.x + pad + std::round(p.px(24.0F));
        const ShapedText t =
            fitter_.fit(p, type::notice_title, h2.title, rx - tx - p.px(8.0F), Elide::End);
        p.draw(t, tx, std::round(cy + (0.5F * t.cap_height())), pal.text_primary);
        float dy = y + fin_h;
        for (std::size_t d = 0; d < h2.details.size() && d < 3; ++d) {
            const ShapedText dt =
                fitter_.fit(p, type::caption, h2.details[d],
                            box.w - (2.0F * pad) - std::round(p.px(24.0F)), Elide::End);
            p.draw(dt, tx, std::round(dy - p.px(10.0F)), pal.text_muted);
            dy += line_h;
        }
        y += fin_h + (static_cast<float>(std::min<std::size_t>(h2.details.size(), 3)) * line_h);
    }
    r.set_clip(std::nullopt);
}

// ---------------------------------------------------------------------------------------------
// Notice
// ---------------------------------------------------------------------------------------------

void OpsView::draw_notice(const OpsFrame& f) {
    const std::optional<OpNotice>& n = f.ops.notice();
    if (!n) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const DensityMetrics dm = density_metrics(f.density);
    const float h = std::round(p.px(40.0F));
    const float pad = std::round(p.px(14.0F));
    const float icon = std::round(p.px(22.0F));
    std::string action_label;
    switch (n->action) {
    case NoticeAction::Undo: action_label = std::string(tr("ops.undo")); break;
    case NoticeAction::Redo: action_label = std::string(tr("ops.redo")); break;
    case NoticeAction::ShowInFolder: action_label = std::string(tr("ops.show_in_folder")); break;
    case NoticeAction::ShowPanel: action_label = std::string(tr("ops.show_details")); break;
    case NoticeAction::None: break;
    }
    const ShapedText al = p.shape(type::button, action_label);
    const float action_w = action_label.empty() ? 0.0F : al.width() + std::round(p.px(22.0F));
    const float close_w = std::round(p.px(28.0F));
    const ShapedText text = p.shape(type::notice_title, n->text);
    const float max_w = std::min(f.width - std::round(p.px(32.0F)), std::round(p.px(640.0F)));
    const float w = std::min(max_w, text.width() + icon + (2.0F * pad) + action_w + close_w);
    const float bottom =
        f.height - std::round(p.px(dm.status_bar_height + 14.0F)) - f.notice_lift_px;
    const RectF box = snap(RectF{std::round((0.5F * f.width) - (0.5F * w)), bottom - h, w, h});
    const float rad = 0.5F * h;
    r.drop_shadow(box, rad, p.px(20.0F), p.px(5.0F), pal.shadow);
    r.fill_rounded_rect(box, rad, n->error ? pal.danger_surface : pal.surface_raised);
    r.stroke_rounded_rect(box, rad, hairline(p), n->error ? pal.danger_border : pal.border);
    next_hits_.push_back(box);
    float x = box.x + pad;
    if (n->error) {
        draw_alert_glyph(r, x + p.px(7.0F), box.center_y(), p.px(7.0F), pal.danger,
                         pal.danger_surface, p.scale());
    } else {
        r.fill_circle(x + p.px(7.0F), box.center_y(), p.px(7.0F), pal.accent.with_alpha(0.18F));
        r.stroke_line(x + p.px(4.0F), box.center_y(), x + p.px(6.3F), box.center_y() + p.px(2.4F),
                      std::max(1.0F, p.px(1.5F)), pal.accent);
        r.stroke_line(x + p.px(6.3F), box.center_y() + p.px(2.4F), x + p.px(10.4F),
                      box.center_y() - p.px(2.6F), std::max(1.0F, p.px(1.5F)), pal.accent);
    }
    x += icon;
    const float right_edge = box.right() - close_w - action_w;
    const ShapedText fitted =
        fitter_.fit(p, type::notice_title, n->text, right_edge - x, Elide::End);
    p.draw(fitted, x, std::round(box.center_y() + (0.5F * fitted.cap_height())), pal.text_primary);
    if (!action_label.empty()) {
        const RectF rr = snap(
            RectF{right_edge, box.center_y() - p.px(12.0F), action_w - p.px(4.0F), p.px(24.0F)});
        const bool hv = contains(rr, f.pointer.x, f.pointer.y);
        if (hv) {
            r.fill_rounded_rect(rr, p.px(radius::sm + 1.0F), pal.control_hover);
        }
        p.draw(al, std::round(rr.center_x() - (0.5F * al.width())),
               std::round(box.center_y() + (0.5F * al.cap_height())), pal.accent);
        if (button(f, rr, 3)) {
            switch (n->action) {
            case NoticeAction::Undo: f.queue.enqueue(CommandId::Undo); break;
            case NoticeAction::Redo: f.queue.enqueue(CommandId::Redo); break;
            case NoticeAction::ShowPanel: f.queue.enqueue(CommandId::ToggleOperationsPanel); break;
            case NoticeAction::ShowInFolder: {
                Intent in;
                in.kind = Intent::Kind::OpenPath;
                in.path = n->folder;
                f.intents.push_back(in);
                break;
            }
            case NoticeAction::None: break;
            }
            Intent d;
            d.kind = Intent::Kind::DismissOpsNotice;
            f.intents.push_back(d);
        }
    }
    const RectF close = snap(RectF{box.right() - close_w, box.y, close_w, box.h});
    const bool hv = contains(close, f.pointer.x, f.pointer.y);
    draw_cross(r, close.center_x() - p.px(2.0F), close.center_y(), p.px(4.0F),
               std::max(1.0F, p.px(1.3F)), hv ? pal.text_primary : pal.text_muted);
    if (button(f, close, 4)) {
        Intent d;
        d.kind = Intent::Kind::DismissOpsNotice;
        f.intents.push_back(d);
    }
}

// ---------------------------------------------------------------------------------------------
// Sheets
// ---------------------------------------------------------------------------------------------

bool OpsView::sheet_button(const OpsFrame& f, const RectF& rect, std::string_view label,
                           bool primary, bool danger, bool focused, std::size_t id) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const bool inside = contains(rect, ptr.x, ptr.y);
    if (inside && ptr.pressed) {
        pressed_ = id;
    }
    const bool mine = pressed_ == id;
    const float rad = std::round(p.px(radius::md - 1.0F));
    const ShapedText t = p.shape(type::button, label);
    Color ink = pal.text_primary;
    if (primary) {
        r.fill_rounded_rect(rect, rad, danger ? pal.danger : pal.accent);
        if (inside) {
            r.fill_rounded_rect(rect, rad, faded(pal.on_accent, mine && ptr.down ? 0.18F : 0.10F));
        }
        ink = pal.on_accent;
    } else {
        r.stroke_rounded_rect(rect, rad, hairline(p), pal.keycap_border);
        if (inside) {
            r.fill_rounded_rect(rect, rad,
                                mine && ptr.down ? pal.control_pressed : pal.control_hover);
        }
    }
    if (focused) {
        // The focus ring sits outside the button (never colour alone: it is a 2 dp outline).
        const float g = std::max(2.0F, std::round(p.px(2.0F)));
        r.stroke_rounded_rect(RectF{rect.x - g - hairline(p), rect.y - g - hairline(p),
                                    rect.w + (2.0F * (g + hairline(p))),
                                    rect.h + (2.0F * (g + hairline(p)))},
                              rad + g, g, pal.focus_ring);
    }
    p.draw(t, std::round(rect.center_x() - (0.5F * t.width())),
           std::round(rect.center_y() + (0.5F * t.cap_height())), ink);
    return mine && ptr.released && inside;
}

float OpsView::keycap_row(const OpsFrame& f, const RectF& area,
                          const std::vector<std::pair<std::string, std::string>>& hints) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    r.fill_rect(RectF{area.x, area.y, area.w, hairline(p)}, pal.border);
    float x = area.x + std::round(p.px(16.0F));
    const float cy = area.center_y() + (0.5F * hairline(p));
    for (const auto& [key, label] : hints) {
        const ShapedText k = p.shape(type::keycap, key);
        const float kh = std::round(p.px(20.0F));
        const float kw = std::round(std::max(kh, k.width() + p.px(12.0F)));
        const ShapedText probe = p.shape(type::caption, label);
        if (x + kw + std::round(p.px(7.0F)) + probe.width() >
            area.right() - std::round(p.px(12.0F))) {
            break; // a hint that does not fit is left out, never drawn past the card
        }
        const RectF cap = snap(RectF{x, cy - (0.5F * kh), kw, kh});
        const float rad = p.px(radius::sm);
        r.fill_rounded_rect(RectF{cap.x, cap.y + hairline(p), cap.w, cap.h}, rad, pal.keycap_edge);
        r.fill_rounded_rect(cap, rad, pal.surface);
        r.stroke_rounded_rect(cap, rad, hairline(p), pal.keycap_border);
        p.draw(k, cap.x + std::round((0.5F * cap.w) - (0.5F * k.width())),
               std::round(cap.center_y() + (0.5F * k.cap_height())), pal.keycap_text);
        const ShapedText t = p.shape(type::caption, label);
        p.draw(t, cap.right() + std::round(p.px(7.0F)), std::round(cy + (0.5F * t.cap_height())),
               pal.text_muted);
        x = cap.right() + std::round(p.px(7.0F)) + t.width() + std::round(p.px(18.0F));
        if (x > area.right()) {
            break;
        }
    }
    return area.h;
}

void OpsView::draw_conflict(const OpsFrame& f) {
    const ConflictSheet* sheet = f.ops.conflict_sheet();
    if (sheet == nullptr) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const ops::Conflict& c = sheet->conflict;
    const std::vector<ConflictButton> buttons = conflict_buttons(c);
    const bool dir = c.existing.kind == fs::EntryKind::Directory;
    const std::string name = fs::to_display_name(c.existing.path.filename().string());

    const float w = std::min(std::round(p.px(640.0F)), f.width - std::round(p.px(32.0F)));
    const float pad = std::round(p.px(20.0F));
    const float head_h = std::round(p.px(58.0F));
    const float side_h = std::round(p.px(134.0F));
    const float name_h = std::round(p.px(56.0F));
    const float apply_h = std::round(p.px(34.0F));
    const float btn_h = std::round(p.px(32.0F));
    const float foot_h = std::round(p.px(36.0F));
    const bool keep_name = std::ranges::find(buttons, ConflictButton::KeepBoth) != buttons.end();
    const float h = head_h + side_h + (keep_name ? name_h : std::round(p.px(10.0F))) + apply_h +
                    btn_h + std::round(p.px(20.0F)) + foot_h;
    const float top = std::max(std::round(p.px(36.0F)), std::round(f.height * 0.12F));
    const RectF box =
        card(f, RectF{std::round((0.5F * f.width) - (0.5F * w)), top, w, h}, 14.0F, false);
    // An accent edge says "this wants an answer" without a scrim (the window stays usable).
    r.stroke_rounded_rect(box, std::round(p.px(14.0F)), std::max(1.0F, std::round(p.px(1.5F))),
                          pal.accent.with_alpha(0.55F));

    // Header.
    draw_alert_glyph(r, box.x + pad + p.px(9.0F), box.y + (0.5F * head_h), p.px(9.0F), pal.warning,
                     pal.surface_raised, p.scale());
    const std::string title_key = c.same_file
                                      ? "conflict.title.same"
                                      : (dir ? "conflict.title.folder" : "conflict.title.file");
    const std::string title = substitute(std::string(tr(title_key)), "name", name);
    const float tx = box.x + pad + std::round(p.px(28.0F));
    const ShapedText tt =
        fitter_.fit(p, type::popup_title, title, box.right() - pad - tx, Elide::Middle);
    p.draw(tt, tx, std::round(box.y + p.px(24.0F)), pal.text_primary);
    std::string sub = op_title(c.kind, 1, name, folder_display(c.existing.path.parent_path()));
    if (c.remaining_items > 1) {
        sub += "  \xC2\xB7  " + tr_count("conflict.waiting", c.remaining_items - 1);
    }
    const ShapedText st = fitter_.fit(p, type::caption, sub, box.right() - pad - tx, Elide::End);
    p.draw(st, tx, std::round(box.y + p.px(43.0F)), pal.text_muted);

    // Sides.
    const float gap = std::round(p.px(12.0F));
    const float side_w = std::round((box.w - (2.0F * pad) - gap) * 0.5F);
    const auto draw_side = [&](const ops::ConflictSide& s, float x0, std::string_view label_key,
                               std::string_view tag_key, bool incoming) {
        const RectF rr = snap(RectF{x0, box.y + head_h, side_w, side_h});
        r.fill_rounded_rect(rr, p.px(radius::md), pal.surface);
        r.stroke_rounded_rect(rr, p.px(radius::md), hairline(p),
                              incoming ? pal.accent.with_alpha(0.45F) : pal.border);
        const ShapedText lab = p.shape(type::section, tr(label_key));
        p.draw(lab, rr.x + p.px(12.0F), std::round(rr.y + p.px(18.0F)), pal.text_muted);
        if (!tag_key.empty()) {
            const ShapedText tg = p.shape(type::caption, tr(tag_key));
            const RectF chip =
                snap(RectF{rr.right() - p.px(12.0F) - tg.width() - p.px(12.0F), rr.y + p.px(7.0F),
                           tg.width() + p.px(12.0F), p.px(18.0F)});
            r.fill_rounded_rect(chip, 0.5F * chip.h, pal.chip);
            p.draw(tg, chip.x + p.px(6.0F), std::round(chip.center_y() + (0.5F * tg.cap_height())),
                   pal.chip_text);
        }
        const float tb = std::round(p.px(80.0F));
        const RectF tbox = snap(RectF{rr.x + p.px(12.0F), rr.y + p.px(34.0F), tb, tb});
        r.fill_rounded_rect(tbox, p.px(radius::sm),
                            pal.row_alt.with_alpha(std::max(0.5F, pal.row_alt.a)));
        const fs::DirEntry e = entry_of_side(s);
        const bool thumb = f.thumb && f.thumb(s.path, e, tbox);
        if (!thumb) {
            const float gs = std::round(p.px(36.0F));
            draw_entry_glyph(r, pal, e, tbox.center_x() - (0.5F * gs),
                             tbox.center_y() - (0.5F * gs), gs, 1.0F, p.scale());
        }
        const float ix = tbox.right() + std::round(p.px(12.0F));
        const float iw = rr.right() - ix - std::round(p.px(10.0F));
        const ShapedText nm =
            fitter_.fit(p, type::notice_title, fs::to_display_name(s.path.filename().string()), iw,
                        Elide::Middle);
        p.draw(nm, ix, std::round(rr.y + p.px(46.0F)), pal.text_primary);
        const bool is_dir = s.kind == fs::EntryKind::Directory;
        const std::string size =
            is_dir ? std::string(tr("conflict.kind.folder")) : format_size(s.size);
        p.draw(p.shape(type::status, size), ix, std::round(rr.y + p.px(68.0F)), pal.text_secondary);
        const std::string date = format_date(s.mtime, f.now);
        p.draw(fitter_.fit(p, type::status, date, iw, Elide::End), ix,
               std::round(rr.y + p.px(88.0F)), pal.text_secondary);
        const std::string kind = s.kind == fs::EntryKind::Symlink
                                     ? std::string(tr("conflict.kind.link"))
                                     : type_label(e);
        p.draw(fitter_.fit(p, type::caption, kind, iw, Elide::End), ix,
               std::round(rr.y + p.px(108.0F)), pal.text_muted);
    };
    const bool newer = c.source_is_newer;
    const bool older = c.source.mtime < c.existing.mtime;
    draw_side(c.source, box.x + pad, "conflict.incoming",
              newer ? "conflict.newer" : (older ? "conflict.older" : ""), true);
    draw_side(c.existing, box.x + pad + side_w + gap, "conflict.existing",
              newer ? "conflict.older" : (older ? "conflict.newer" : ""), false);

    float y = box.y + head_h + side_h;
    if (c.same_file) {
        const ShapedText t = p.shape(type::caption, tr("conflict.same_hint"));
        p.draw(t, box.x + pad, std::round(y + p.px(18.0F)), pal.text_muted);
    }
    if (keep_name) {
        const ShapedText lab = p.shape(type::section, tr("conflict.name_label"));
        p.draw(lab, box.x + pad, std::round(y + p.px(20.0F)), pal.text_muted);
        const RectF field =
            snap(RectF{box.x + pad, y + p.px(26.0F), box.w - (2.0F * pad), p.px(28.0F)});
        const float frad = std::round(p.px(radius::md - 1.0F));
        r.fill_rounded_rect(field, frad, pal.surface);
        const bool bad = !sheet->name_error.empty();
        r.stroke_rounded_rect(
            field, frad, std::max(1.0F, std::round(p.px(sheet->editing_name ? 1.5F : 1.0F))),
            bad ? pal.danger : (sheet->editing_name ? pal.focus_ring : pal.border));
        TextFieldStyle style;
        style.text = &type::row;
        style.caret_height_dp = 15.0F;
        style.show_caret = sheet->editing_name;
        (void)draw_text_field(p, pal, sheet->name,
                              RectF{field.x + p.px(10.0F), field.y, field.w - p.px(20.0F), field.h},
                              {}, conflict_name_scroll_, style);
        if (button(f, field, 220)) {
            Intent in;
            in.kind = Intent::Kind::SheetEditName;
            f.intents.push_back(in);
        }
        if (bad) {
            const ShapedText et = p.shape(type::caption, sheet->name_error);
            p.draw(et, box.right() - pad - et.width(), std::round(y + p.px(20.0F)), pal.danger);
        }
        y += name_h;
    } else {
        y += std::round(p.px(10.0F));
    }

    // Apply to all.
    {
        const RectF cb = snap(RectF{box.x + pad, y + p.px(9.0F), p.px(16.0F), p.px(16.0F)});
        const std::string label =
            c.remaining_items > 1
                ? substitute(tr_count("conflict.apply_all_more", c.remaining_items - 1), "n",
                             format_count(c.remaining_items - 1))
                : std::string(tr("conflict.apply_all"));
        const ShapedText lt = p.shape(type::row, label);
        const RectF hitr = snap(RectF{cb.x, y + p.px(4.0F), p.px(30.0F) + lt.width(), p.px(26.0F)});
        r.stroke_rounded_rect(cb, p.px(4.0F), std::max(1.0F, std::round(p.px(1.5F))),
                              sheet->apply_all ? pal.accent : pal.control_border);
        if (sheet->apply_all) {
            r.fill_rounded_rect(cb, p.px(4.0F), pal.accent);
            r.stroke_line(cb.x + p.px(3.5F), cb.center_y(), cb.x + p.px(6.5F),
                          cb.center_y() + p.px(3.0F), std::max(1.0F, p.px(1.6F)), pal.on_accent);
            r.stroke_line(cb.x + p.px(6.5F), cb.center_y() + p.px(3.0F), cb.x + p.px(12.5F),
                          cb.center_y() - p.px(3.5F), std::max(1.0F, p.px(1.6F)), pal.on_accent);
        }
        p.draw(lt, cb.right() + p.px(9.0F), std::round(cb.center_y() + (0.5F * lt.cap_height())),
               pal.text_primary);
        if (button(f, hitr, 221)) {
            Intent in;
            in.kind = Intent::Kind::SheetApplyAll;
            f.intents.push_back(in);
        }
        y += apply_h;
    }

    // Buttons, right-aligned; Cancel operation sits apart on the left.
    {
        const float by = y + p.px(2.0F);
        float x = box.right() - pad;
        for (std::size_t i = buttons.size(); i-- > 0;) {
            const ConflictButton b = buttons[i];
            if (b == ConflictButton::Cancel) {
                continue;
            }
            const std::string label(tr(button_label_key(b, c)));
            const ShapedText lt = p.shape(type::button, label);
            const float bw = std::round(lt.width() + p.px(28.0F));
            const RectF rr = snap(RectF{x - bw, by, bw, btn_h});
            const bool primary = b == ConflictButton::KeepBoth;
            const bool danger = b == ConflictButton::Replace && c.replace_is_permanent;
            if (sheet_button(f, rr, label, primary, danger && false, sheet->focus == i, 230 + i)) {
                Intent in;
                in.kind = Intent::Kind::SheetButton;
                in.index = i;
                f.intents.push_back(in);
            }
            x = rr.x - std::round(p.px(10.0F));
        }
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            if (buttons[i] != ConflictButton::Cancel) {
                continue;
            }
            const std::string label(tr("conflict.cancel"));
            const ShapedText lt = p.shape(type::button, label);
            const float bw = std::round(lt.width() + p.px(28.0F));
            const RectF rr = snap(RectF{box.x + pad, by, bw, btn_h});
            if (sheet_button(f, rr, label, false, false, sheet->focus == i, 230 + i)) {
                Intent in;
                in.kind = Intent::Kind::SheetButton;
                in.index = i;
                f.intents.push_back(in);
            }
        }
    }
    keycap_row(f, RectF{box.x, box.bottom() - foot_h, box.w, foot_h},
               {{"Enter", std::string(tr("conflict.hint.activate"))},
                {"Tab", std::string(tr("conflict.hint.cycle"))},
                {"Space", std::string(tr("conflict.hint.all"))},
                {"F2", std::string(tr("conflict.hint.name"))},
                {"Esc", std::string(tr("conflict.hint.cancel"))}});
}

void OpsView::draw_confirm(const OpsFrame& f) {
    const ConfirmSheet* sheet = f.ops.confirm_sheet();
    if (sheet == nullptr) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float w = std::min(std::round(p.px(460.0F)), f.width - std::round(p.px(32.0F)));
    const float pad = std::round(p.px(22.0F));
    const float text_w = w - (2.0F * pad) - std::round(p.px(36.0F));

    std::string title;
    std::string body;
    std::string button_label;
    switch (sheet->kind) {
    case SheetKind::DeletePermanently:
    case SheetKind::DeleteTrashed:
        title = sheet->count == 1
                    ? substitute(std::string(tr("confirm.delete.title.one")), "name",
                                 sheet->names.empty() ? std::string() : sheet->names.front())
                    : tr_count("confirm.delete.title", sheet->count);
        body = std::string(tr("confirm.delete.body"));
        button_label = std::string(tr("confirm.delete.button"));
        break;
    case SheetKind::DeleteUntrashable:
        title = sheet->count == 1
                    ? substitute(std::string(tr("confirm.untrashable.title.one")), "name",
                                 sheet->names.empty() ? std::string() : sheet->names.front())
                    : tr_count("confirm.untrashable.title", sheet->count);
        body = std::string(tr("confirm.untrashable.body"));
        button_label = std::string(tr("confirm.delete.button"));
        break;
    case SheetKind::EmptyTrash:
        title = std::string(tr("confirm.empty.title"));
        body = tr_count("confirm.empty.body", sheet->count);
        button_label = std::string(tr("confirm.empty.button"));
        break;
    case SheetKind::None:
    case SheetKind::Conflict:
    case SheetKind::QuitRunning: return;
    }
    const std::vector<std::string> body_lines = wrap_lines(p, type::body, body, text_w);
    const float line_h = std::round(p.px(20.0F));
    const bool list_names = sheet->kind != SheetKind::EmptyTrash && sheet->count > 1;
    const std::size_t shown = list_names ? std::min<std::size_t>(sheet->names.size(), 4) : 0;
    const bool more = list_names && sheet->count > shown;
    const bool stats = sheet->probe != nullptr;
    const float names_h = static_cast<float>(shown + (more ? 1 : 0)) * std::round(p.px(18.0F));
    const float h = std::round(p.px(24.0F)) + std::round(p.px(26.0F)) +
                    (static_cast<float>(body_lines.size()) * line_h) +
                    (names_h > 0.0F ? names_h + std::round(p.px(10.0F)) : 0.0F) +
                    (stats ? std::round(p.px(26.0F)) : 0.0F) + std::round(p.px(36.0F)) +
                    std::round(p.px(22.0F)) + std::round(p.px(36.0F));
    const float top =
        std::max(std::round(p.px(48.0F)), std::round((0.5F * f.height) - (0.62F * h)));
    const RectF box =
        card(f, RectF{std::round((0.5F * f.width) - (0.5F * w)), top, w, h}, 14.0F, true);
    modal_ = true;

    float y = box.y + std::round(p.px(24.0F));
    draw_alert_glyph(r, box.x + pad + p.px(11.0F), y + p.px(10.0F), p.px(11.0F), pal.danger,
                     pal.surface_raised, p.scale());
    const float tx = box.x + pad + std::round(p.px(36.0F));
    const ShapedText tt = fitter_.fit(p, type::popup_title, title, text_w, Elide::Middle);
    p.draw(tt, tx, std::round(y + p.px(14.0F)), pal.text_primary);
    y += std::round(p.px(26.0F));
    for (const std::string& line : body_lines) {
        y += line_h;
        p.draw(p.shape(type::body, line), tx, std::round(y - p.px(5.0F)), pal.text_secondary);
    }
    if (names_h > 0.0F) {
        y += std::round(p.px(10.0F));
        for (std::size_t i = 0; i < shown; ++i) {
            y += std::round(p.px(18.0F));
            const ShapedText nt = fitter_.fit(p, type::row, "\xE2\x80\xA2  " + sheet->names[i],
                                              text_w, Elide::Middle);
            p.draw(nt, tx, std::round(y - p.px(4.0F)), pal.text_primary);
        }
        if (more) {
            y += std::round(p.px(18.0F));
            p.draw(p.shape(type::caption, tr_count("confirm.more", sheet->count - shown)), tx,
                   std::round(y - p.px(4.0F)), pal.text_muted);
        }
    }
    if (stats) {
        y += std::round(p.px(26.0F));
        std::string st(tr("confirm.stats_counting"));
        if (const std::optional<TreeStats> s = f.ops.confirm_stats(); s) {
            const std::uint64_t n = s->files + s->folders;
            if (s->done || n > 0) {
                st = s->partial
                         ? substitute(substitute(std::string(tr("confirm.stats_partial")), "n",
                                                 format_count(n)),
                                      "size", format_size(s->bytes))
                         : substitute(tr_count("confirm.stats", n), "size", format_size(s->bytes));
            }
            animating_ = animating_ || !s->done;
        }
        p.draw(p.shape(type::status, st), tx, std::round(y - p.px(8.0F)), pal.text_secondary);
    }
    y = box.bottom() - std::round(p.px(36.0F)) - std::round(p.px(22.0F)) - std::round(p.px(36.0F)) +
        std::round(p.px(20.0F));
    // Buttons: Cancel (focus starts here) and the destructive one.
    const float btn_h = std::round(p.px(32.0F));
    const ShapedText dl = p.shape(type::button, button_label);
    const float dw = std::round(dl.width() + p.px(32.0F));
    const RectF del = snap(RectF{box.right() - pad - dw, y, dw, btn_h});
    const std::string cancel_label(tr("confirm.cancel"));
    const float cw = std::round(p.shape(type::button, cancel_label).width() + p.px(32.0F));
    const RectF can = snap(RectF{del.x - std::round(p.px(10.0F)) - cw, y, cw, btn_h});
    if (sheet_button(f, can, cancel_label, false, false, sheet->focus == 0, 300)) {
        Intent in;
        in.kind = Intent::Kind::SheetButton;
        in.index = 0;
        f.intents.push_back(in);
    }
    if (sheet_button(f, del, button_label, true, true, sheet->focus == 1, 301)) {
        Intent in;
        in.kind = Intent::Kind::SheetButton;
        in.index = 1;
        f.intents.push_back(in);
    }
    keycap_row(f,
               RectF{box.x, box.bottom() - std::round(p.px(36.0F)), box.w, std::round(p.px(36.0F))},
               {{"Tab", std::string(tr("conflict.hint.cycle"))},
                {"Enter", std::string(tr("conflict.hint.activate"))},
                {"Esc", std::string(tr("confirm.cancel"))}});
}

void OpsView::draw_quit(const OpsFrame& f) {
    if (f.ops.sheet_kind() != SheetKind::QuitRunning) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float w = std::min(std::round(p.px(500.0F)), f.width - std::round(p.px(32.0F)));
    const float pad = std::round(p.px(22.0F));
    const float text_w = w - (2.0F * pad) - std::round(p.px(36.0F));
    const std::string body = tr_count("quit.body", f.ops.running().size());
    const std::vector<std::string> lines = wrap_lines(p, type::body, body, text_w);
    const float line_h = std::round(p.px(20.0F));
    const float h = std::round(p.px(24.0F + 26.0F + 16.0F + 36.0F + 22.0F + 36.0F)) +
                    (static_cast<float>(lines.size()) * line_h);
    const float top =
        std::max(std::round(p.px(48.0F)), std::round((0.5F * f.height) - (0.62F * h)));
    const RectF box =
        card(f, RectF{std::round((0.5F * f.width) - (0.5F * w)), top, w, h}, 14.0F, true);
    modal_ = true;
    float y = box.y + std::round(p.px(24.0F));
    draw_alert_glyph(r, box.x + pad + p.px(11.0F), y + p.px(10.0F), p.px(11.0F), pal.warning,
                     pal.surface_raised, p.scale());
    const float tx = box.x + pad + std::round(p.px(36.0F));
    p.draw(p.shape(type::popup_title, tr("quit.title")), tx, std::round(y + p.px(14.0F)),
           pal.text_primary);
    y += std::round(p.px(26.0F));
    for (const std::string& line : lines) {
        y += line_h;
        p.draw(p.shape(type::body, line), tx, std::round(y - p.px(5.0F)), pal.text_secondary);
    }
    const float by =
        box.bottom() - std::round(p.px(36.0F + 22.0F + 36.0F)) + std::round(p.px(20.0F));
    const float btn_h = std::round(p.px(32.0F));
    const std::array<std::string, 3> labels{std::string(tr("quit.keep")),
                                            std::string(tr("quit.wait")),
                                            std::string(tr("quit.cancel_quit"))};
    float x = box.right() - pad;
    for (std::size_t i = 3; i-- > 0;) {
        const float bw = std::round(p.shape(type::button, labels[i]).width() + p.px(28.0F));
        const RectF rr = snap(RectF{x - bw, by, bw, btn_h});
        if (sheet_button(f, rr, labels[i], i == 0, false, f.ops.quit_focus() == i, 310 + i)) {
            Intent in;
            in.kind = Intent::Kind::SheetButton;
            in.index = i;
            f.intents.push_back(in);
        }
        x = rr.x - std::round(p.px(8.0F));
    }
    keycap_row(f,
               RectF{box.x, box.bottom() - std::round(p.px(36.0F)), box.w, std::round(p.px(36.0F))},
               {{"Tab", std::string(tr("conflict.hint.cycle"))},
                {"Enter", std::string(tr("conflict.hint.activate"))},
                {"Esc", std::string(tr("quit.keep"))}});
}

// ---------------------------------------------------------------------------------------------
// Drag visuals
// ---------------------------------------------------------------------------------------------

void OpsView::draw_drag(const OpsFrame& f) {
    const DragVisual* d = f.drag;
    if (d == nullptr || !d->active) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const DropEval& ev = d->eval;
    const bool ok = ev.allowed();

    // Target highlight: the folder row, the pane, the crumb, the tab, the sidebar row.
    if (ev.rect.w > 0.0F && d->pointer_inside) {
        const Color accent = ok ? pal.accent : pal.danger;
        const float rad = std::round(p.px(ev.kind == SlotKind::Pane ? radius::lg - 2.0F : 6.0F));
        const float t = std::max(1.5F, std::round(p.px(1.5F)));
        RectF rr = ev.rect;
        switch (ev.kind) {
        case SlotKind::Pane:
            rr = RectF{rr.x + p.px(2.0F), rr.y + p.px(2.0F), rr.w - p.px(4.0F), rr.h - p.px(4.0F)};
            r.fill_rounded_rect(rr, rad, accent.with_alpha(ok ? 0.07F : 0.04F));
            r.stroke_rounded_rect(rr, rad, t * 1.4F, accent.with_alpha(ok ? 0.75F : 0.5F));
            break;
        case SlotKind::Row:
            r.fill_rounded_rect(rr, rad, accent.with_alpha(ok ? 0.20F : 0.10F));
            r.stroke_rounded_rect(rr, rad, t, accent.with_alpha(ok ? 0.9F : 0.6F));
            break;
        case SlotKind::Crumb:
        case SlotKind::Tab:
        case SlotKind::Sidebar:
        case SlotKind::Trash:
            r.fill_rounded_rect(rr, rad, accent.with_alpha(ok ? 0.22F : 0.10F));
            r.stroke_rounded_rect(rr, rad, t, accent.with_alpha(ok ? 0.9F : 0.6F));
            break;
        }
    }

    // Badge next to the pointer.
    std::string action;
    if (!d->pointer_inside && !d->outgoing_note.empty()) {
        action = d->outgoing_note;
    } else if (ev.trash) {
        action = std::string(tr("drag.trash"));
    } else {
        switch (ev.action) {
        case transfer::DropAction::Copy: action = std::string(tr("drag.copy")); break;
        case transfer::DropAction::Move: action = std::string(tr("drag.move")); break;
        case transfer::DropAction::Link: action = std::string(tr("drag.link")); break;
        case transfer::DropAction::Ask: action = std::string(tr("drag.ask")); break;
        case transfer::DropAction::None:
            action = ev.rect.w > 0.0F ? std::string(tr("drag.none")) : std::string();
            break;
        }
        if (ok && !ev.label.empty() && ev.action != transfer::DropAction::Ask) {
            action += "  \xC2\xB7  " + substitute(std::string(tr("drag.into")), "folder", ev.label);
        }
    }
    const std::string name = fs::to_display_name(d->first.name);
    const bool has_items = d->count > 0 && !name.empty();
    const float pad = std::round(p.px(10.0F));
    const float icon = has_items ? std::round(p.px(18.0F)) : 0.0F;
    const ShapedText nm = fitter_.fit(p, type::notice_title, name, p.px(190.0F), Elide::Middle);
    const ShapedText ac = p.shape(type::caption, action);
    const float body_w = has_items ? std::max(nm.width(), ac.width()) : ac.width();
    const float w = std::round(pad + icon + (has_items ? p.px(9.0F) : 0.0F) + body_w + pad +
                               (d->count > 1 ? p.px(22.0F) : 0.0F));
    const float h = std::round(!has_items || action.empty() ? p.px(30.0F) : p.px(44.0F));
    float bx = d->x + p.px(16.0F);
    float by = d->y + p.px(14.0F);
    bx = std::clamp(bx, p.px(4.0F), std::max(p.px(4.0F), f.width - w - p.px(4.0F)));
    by = std::clamp(by, p.px(4.0F), std::max(p.px(4.0F), f.height - h - p.px(4.0F)));
    const RectF box = snap(RectF{bx, by, w, h});
    const float rad = std::round(p.px(10.0F));
    r.drop_shadow(box, rad, p.px(18.0F), p.px(5.0F), pal.shadow);
    r.fill_rounded_rect(box, rad, pal.surface_raised.with_alpha(0.97F));
    r.stroke_rounded_rect(box, rad, hairline(p),
                          ok || action.empty() ? pal.border : pal.danger_border);
    if (has_items) {
        draw_entry_glyph(r, pal, d->first, box.x + pad, box.y + std::round(p.px(7.0F)), icon, 1.0F,
                         p.scale());
        const float tx = box.x + pad + icon + std::round(p.px(9.0F));
        p.draw(nm, tx, std::round(box.y + p.px(19.0F)), pal.text_primary);
        if (!action.empty()) {
            p.draw(ac, tx, std::round(box.y + p.px(36.0F)), ok ? pal.text_secondary : pal.danger);
        }
    } else if (!action.empty()) {
        p.draw(ac, box.x + pad, std::round(box.center_y() + (0.5F * ac.cap_height())),
               ok ? pal.text_primary : pal.danger);
    }
    if (d->count > 1) {
        const std::string n = std::to_string(d->count);
        const ShapedText nt = p.shape(type::keycap, n);
        const float bw = std::max(std::round(p.px(18.0F)), nt.width() + std::round(p.px(10.0F)));
        const RectF badge =
            snap(RectF{box.right() - bw - p.px(8.0F), box.y + p.px(6.0F), bw, p.px(18.0F)});
        r.fill_rounded_rect(badge, 0.5F * badge.h, pal.accent);
        p.draw(nt, std::round(badge.center_x() - (0.5F * nt.width())),
               std::round(badge.center_y() + (0.5F * nt.cap_height())), pal.on_accent);
    }
}

// ---------------------------------------------------------------------------------------------

void OpsView::draw_overlays(const OpsFrame& f) {
    animating_ = false;
    modal_ = false;
    hits_ = std::move(next_hits_); // the chip registered its rect earlier in the frame
    next_hits_.clear();
    // Hits registered this frame by the chip are in `hits_`; overlays add to next_hits_.
    const std::vector<RectF> chip_hits = hits_;
    draw_panel(f);
    draw_notice(f);
    const SheetKind kind = f.ops.sheet_kind();
    switch (kind) {
    case SheetKind::Conflict: draw_conflict(f); break;
    case SheetKind::QuitRunning: draw_quit(f); break;
    case SheetKind::DeletePermanently:
    case SheetKind::DeleteUntrashable:
    case SheetKind::EmptyTrash:
    case SheetKind::DeleteTrashed: draw_confirm(f); break;
    case SheetKind::None: break;
    }
    draw_drag(f);
    for (const RectF& h : chip_hits) {
        next_hits_.push_back(h);
    }
    hits_ = next_hits_;
    next_hits_.clear();
    if (!f.ops.running().empty()) {
        animating_ = true;
    }
    if (f.pointer.released) {
        pressed_.reset();
    }
}

} // namespace navigator::ui
