// src/ui/inspector/inspector_view.cpp — the Inspector pane. See inspector_view.hpp.
#include "inspector/inspector_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>

#include <navigator/i18n.hpp>
#include <navigator/mime.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "widgets/icons.hpp"

namespace navigator::ui::inspector {

namespace {

constexpr double kFadeMs = 180.0;
constexpr float kHeaderH = 50.0F;    // dp
constexpr float kButton = 28.0F;     // dp
constexpr float kMetaRowH = 22.0F;   // dp
constexpr float kFolderRowH = 22.0F; // dp
constexpr float kCheckerCell = 8.0F; // dp

enum Glyph : int {
    GLock = 0,
    GPlaceRight = 1,
    GPlaceBottom = 2,
    GPlaceAuto = 3,
    GFit = 4,
    GOne = 5,
    GGrid = 6,
    GMinimap = 7,
    GPlay = 8,
    GStack = 9,
    GAlert = 10,
    GEntry = 11,
    GEmpty = 12,
};

// Opaque `overlay` over `base` (translucent palette overlays composited once).
Color over(const Color& base, const Color& overlay) {
    return mix(base, overlay.with_alpha(1.0F), overlay.a);
}

Color well_color(const Palette& pal, Theme theme) {
    return theme == Theme::Dark ? mix(pal.surface_raised, rgb(0x000000), 0.28F)
                                : mix(pal.surface_raised, rgb(0x000000), 0.045F);
}

void checker(Renderer& r, const RectF& area, float cell, Theme theme) {
    const Color a = theme == Theme::Dark ? rgb(0x2B2D31) : rgb(0xF2F2F2);
    const Color b = theme == Theme::Dark ? rgb(0x37393E) : rgb(0xDCDCDC);
    r.fill_rect(area, a);
    if (cell < 2.0F) {
        return;
    }
    const auto cols = static_cast<int>(std::ceil(area.w / cell));
    const auto rows = static_cast<int>(std::ceil(area.h / cell));
    for (int y = 0; y < rows; ++y) {
        for (int x = (y % 2); x < cols; x += 2) {
            const float x0 = area.x + (static_cast<float>(x) * cell);
            const float y0 = area.y + (static_cast<float>(y) * cell);
            r.fill_rect(RectF{x0, y0, std::min(cell, area.right() - x0),
                              std::min(cell, area.bottom() - y0)},
                        b);
        }
    }
}

std::string error_detail_key(ErrorCode code) {
    switch (code) {
    case ErrorCode::PermissionDenied: return "inspector.error.permission";
    case ErrorCode::NotFound: return "inspector.error.not_found";
    case ErrorCode::OutOfMemory: return "inspector.error.too_large";
    case ErrorCode::InvalidArgument: return "inspector.error.damaged";
    case ErrorCode::Unsupported: return "inspector.unsupported_detail";
    default: return "inspector.error.generic";
    }
}

RectF inset(const RectF& r, float dx, float dy) {
    return RectF{r.x + dx, r.y + dy, std::max(0.0F, r.w - (2.0F * dx)),
                 std::max(0.0F, r.h - (2.0F * dy))};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

void InspectorView::zoom_fit() noexcept { viewport_.fit(); }

void InspectorView::zoom_1to1() noexcept { viewport_.one_to_one(); }

void InspectorView::cursor(CommandId id) noexcept {
    const bool image = !viewport_.empty() && media_.w > 0.0F && viewport_.pannable();
    const double page = std::max(40.0, text_page_ - 40.0);
    const auto scroll = [](double& pos, double max, double delta) {
        pos = std::clamp(pos + delta, 0.0, std::max(0.0, max));
    };
    double& target = text_max_ > 0.0 ? text_scroll_ : meta_scroll_;
    const double max = text_max_ > 0.0 ? text_max_ : meta_max_;
    switch (id) {
    case CommandId::CursorUp:
        if (image) {
            viewport_.pan_steps(0, -1);
        } else {
            scroll(target, max, -40.0);
        }
        break;
    case CommandId::CursorDown:
        if (image) {
            viewport_.pan_steps(0, 1);
        } else {
            scroll(target, max, 40.0);
        }
        break;
    case CommandId::CursorPageUp:
        if (image) {
            viewport_.pan_steps(0, -6);
        } else {
            scroll(target, max, -page);
        }
        break;
    case CommandId::CursorPageDown:
        if (image) {
            viewport_.pan_steps(0, 6);
        } else {
            scroll(target, max, page);
        }
        break;
    case CommandId::CursorHome:
        if (image) {
            viewport_.pan_steps(-1000, -1000);
        } else {
            target = 0.0;
        }
        break;
    case CommandId::CursorEnd:
        if (image) {
            viewport_.pan_steps(1000, 1000);
        } else {
            target = max;
        }
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

void InspectorView::handle_splitter(const InspectorFrame& f, InspectorOutput& out) {
    const PointerInput& ptr = f.pointer;
    resize_hot_ = active_ == Hot::None && ptr.inside && contains(f.splitter, ptr.x, ptr.y);
    if (resize_hot_ && ptr.pressed) {
        active_ = Hot::Splitter;
        resizing_ = true;
    }
    if (resizing_) {
        if (ptr.down || ptr.released) {
            const float pos = f.placement == layout::InspectorPlacement::Bottom ? ptr.y : ptr.x;
            out.resize = drag_extent(f.placement, f.content, f.painter.scale(), pos);
        }
        if (!ptr.down) {
            resizing_ = false;
            active_ = Hot::None;
        }
    }
}

void InspectorView::draw(const InspectorFrame& f, InspectorOutput& out) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    animating_ = false;
    tooltip_.clear();

    handle_splitter(f, out);

    // Card: inset like the pane panels, on the raised surface with the popup border.
    const DensityMetrics dm = density_metrics(f.density);
    const float margin = std::round(p.px(dm.panel_margin));
    const bool right = f.placement == layout::InspectorPlacement::Right;
    RectF card = f.dock;
    if (right) {
        card = RectF{card.x + std::round(0.5F * margin), card.y + margin,
                     card.w - std::round(1.5F * margin), card.h - (2.0F * margin)};
    } else {
        card = RectF{card.x + margin, card.y + std::round(0.5F * margin), card.w - (2.0F * margin),
                     card.h - std::round(1.5F * margin)};
    }
    card = snap(card);
    card_ = card;
    if (card.w < p.px(60.0F) || card.h < p.px(60.0F)) {
        return;
    }
    const float rad = std::round(p.px(radius::lg));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    r.drop_shadow(card, rad, p.px(14.0F), p.px(3.0F), pal.shadow.with_alpha(pal.shadow.a * 0.55F));
    r.fill_rounded_rect(card, rad, pal.surface_raised);
    if (ptr.pressed && contains(card, ptr.x, ptr.y) && active_ != Hot::Splitter) {
        out.pressed_inside = true;
    }

    // Content changed: reset the per-item view state.
    if (f.controller.serial() != seen_serial_) {
        seen_serial_ = f.controller.serial();
        const Item* item = f.controller.item();
        const Path path = item != nullptr ? item->path : Path();
        if (path != shown_path_) {
            shown_path_ = path;
            text_scroll_ = 0.0;
            meta_scroll_ = 0.0;
            shown_key_.reset();
            viewport_.set_image(0.0, 0.0);
            fade_start_ms_ = -1.0;
        }
        meta_serial_ = 0; // rebuild metadata
    }

    const float pad = std::round(p.px(space::md));
    const RectF header{card.x + pad, card.y, card.w - (2.0F * pad), std::round(p.px(kHeaderH))};
    draw_header(f, header);
    r.fill_rect(RectF{card.x + hair, header.bottom() - hair, card.w - (2.0F * hair), hair},
                pal.border);
    const RectF body{card.x + hair, header.bottom(), card.w - (2.0F * hair),
                     card.bottom() - header.bottom() - hair};
    r.set_clip(body);
    draw_body(f, body, out);
    r.set_clip(std::nullopt);

    // Border last: crisp over the content; the focus ring while the Inspector has the keyboard.
    const bool ring = f.keyboard && f.window_focused;
    r.stroke_rounded_rect(card, rad, ring ? std::max(hair, std::round(p.px(1.5F))) : hair,
                          ring ? pal.focus_ring : pal.border);

    // Splitter feedback.
    if (resize_hot_ || resizing_) {
        const float t = std::max(2.0F, std::round(p.px(2.0F)));
        const Color c = pal.accent.with_alpha(resizing_ ? 0.9F : 0.55F);
        const float in = std::round(p.px(14.0F));
        if (right) {
            r.fill_rounded_rect(RectF{std::round(card.x - (0.5F * margin) - (0.5F * t)),
                                      card.y + in, t, card.h - (2.0F * in)},
                                0.5F * t, c);
        } else {
            r.fill_rounded_rect(RectF{card.x + in,
                                      std::round(card.y - (0.5F * margin) - (0.5F * t)),
                                      card.w - (2.0F * in), t},
                                0.5F * t, c);
        }
    }
    if (!ptr.down && active_ != Hot::Splitter) {
        active_ = Hot::None;
    }
    if (resizing_) {
        animating_ = true;
    }
}

// ---------------------------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------------------------

bool InspectorView::icon_button(const InspectorFrame& f, const RectF& rect, Hot id, int glyph,
                                bool on, bool enabled) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const bool inside = enabled && ptr.inside && contains(rect, ptr.x, ptr.y);
    if (inside && ptr.pressed && active_ == Hot::None) {
        active_ = id;
    }
    const bool hover = inside && (active_ == Hot::None || active_ == id);
    const bool fired = active_ == id && ptr.released && inside;
    const float rad = std::round(p.px(radius::md - 1.0F));
    if (on) {
        r.fill_rounded_rect(rect, rad, pal.chip);
    }
    if (hover) {
        r.fill_rounded_rect(rect, rad, active_ == id ? pal.control_pressed : pal.control_hover);
    }
    const Color ink = !enabled ? pal.text_muted
                      : on     ? pal.chip_text
                      : hover  ? pal.text_primary
                               : pal.text_secondary;
    const float s = std::round(p.px(15.0F));
    const float x = std::round(rect.center_x() - (0.5F * s));
    const float y = std::round(rect.center_y() - (0.5F * s));
    const float t = std::max(1.0F, p.px(1.35F));
    const float u = s / 16.0F;
    switch (glyph) {
    case GLock:
        draw_line_icon(r, LineIcon::Lock, x, y, s, t, ink,
                       on ? over(pal.surface_raised, pal.chip) : pal.surface_raised, false);
        break;
    case GPlaceRight:
    case GPlaceBottom:
    case GPlaceAuto: {
        const RectF frame{x + u, y + (2.0F * u), 14.0F * u, 12.0F * u};
        r.stroke_rounded_rect(frame, 2.0F * u, t, ink);
        if (glyph == GPlaceRight) {
            r.fill_rounded_rect(
                RectF{frame.x + (8.5F * u), frame.y + (2.0F * u), 3.5F * u, 8.0F * u}, u, ink);
        } else if (glyph == GPlaceBottom) {
            r.fill_rounded_rect(
                RectF{frame.x + (2.0F * u), frame.y + (6.5F * u), 10.0F * u, 3.5F * u}, u, ink);
        } else {
            r.fill_rounded_rect(
                RectF{frame.x + (8.5F * u), frame.y + (2.0F * u), 3.5F * u, 3.5F * u}, u, ink);
            r.fill_rounded_rect(
                RectF{frame.x + (2.0F * u), frame.y + (6.5F * u), 10.0F * u, 3.5F * u}, u,
                ink.with_alpha(ink.a * 0.55F));
        }
        break;
    }
    case GFit: {
        // Four corner brackets.
        const float a = 4.5F * u;
        const float x0 = x + (1.5F * u);
        const float y0 = y + (1.5F * u);
        const float x1 = x + (14.5F * u);
        const float y1 = y + (14.5F * u);
        r.stroke_line(x0, y0, x0 + a, y0, t, ink);
        r.stroke_line(x0, y0, x0, y0 + a, t, ink);
        r.stroke_line(x1, y0, x1 - a, y0, t, ink);
        r.stroke_line(x1, y0, x1, y0 + a, t, ink);
        r.stroke_line(x0, y1, x0 + a, y1, t, ink);
        r.stroke_line(x0, y1, x0, y1 - a, t, ink);
        r.stroke_line(x1, y1, x1 - a, y1, t, ink);
        r.stroke_line(x1, y1, x1, y1 - a, t, ink);
        break;
    }
    case GOne: {
        const ShapedText lbl = p.shape(TextStyle{11.5F, FontWeight::SemiBold, 0.0F, true}, "1:1");
        p.draw(lbl, std::round(rect.center_x() - (0.5F * lbl.width())),
               std::round(rect.center_y() + (0.5F * lbl.cap_height())), ink);
        break;
    }
    case GGrid: {
        const RectF g{x + (1.5F * u), y + (1.5F * u), 13.0F * u, 13.0F * u};
        r.stroke_rounded_rect(g, 1.5F * u, t, ink);
        for (int i = 1; i < 3; ++i) {
            const float o = (13.0F * u * static_cast<float>(i)) / 3.0F;
            r.fill_rect(RectF{std::round(g.x + o - (0.5F * t)), g.y, t, g.h}, ink);
            r.fill_rect(RectF{g.x, std::round(g.y + o - (0.5F * t)), g.w, t}, ink);
        }
        break;
    }
    case GMinimap: {
        const RectF g{x + (1.0F * u), y + (2.0F * u), 14.0F * u, 12.0F * u};
        r.stroke_rounded_rect(g, 2.0F * u, t, ink);
        r.fill_rounded_rect(RectF{g.x + (7.0F * u), g.y + (6.0F * u), 5.0F * u, 4.0F * u}, u, ink);
        break;
    }
    default: break;
    }
    return fired;
}

void InspectorView::draw_header(const InspectorFrame& f, const RectF& area) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const Controller& c = f.controller;
    const float b = std::round(p.px(kButton));
    const float gap = std::round(p.px(2.0F));
    float right = area.right();
    const float by = std::round(area.center_y() - (0.5F * b));

    // Placement (cycles Auto -> Right -> Bottom) and lock.
    const int place_glyph = f.state.placement == layout::InspectorPlacement::Auto    ? GPlaceAuto
                            : f.state.placement == layout::InspectorPlacement::Right ? GPlaceRight
                                                                                     : GPlaceBottom;
    RectF br{right - b, by, b, b};
    if (icon_button(f, br, Hot::Placement, place_glyph, false)) {
        f.queue.enqueue(CommandId::CycleInspectorPlacement);
    }
    right = br.x - gap;
    br = RectF{right - b, by, b, b};
    const bool can_lock = c.locked() || (c.item() != nullptr && c.status() != Status::Multi);
    if (icon_button(f, br, Hot::Lock, GLock, c.locked(), can_lock)) {
        f.queue.enqueue(CommandId::ToggleInspectorLock);
    }
    right = br.x - gap;

    // "Play" for videos (a Navigator extra): opens the default player.
    const Item* item = c.item();
    const bool multi = c.status() == Status::Multi;
    const std::string_view mime =
        item != nullptr ? mime::guess_for_entry(item->entry) : std::string_view();
    if (item != nullptr && !multi && mime.starts_with("video/")) {
        const ShapedText lbl = p.shape(type::button, tr("inspector.play"));
        const float w = std::round(lbl.width() + p.px(34.0F));
        const RectF pr{right - w - p.px(4.0F), by, w, b};
        const PointerInput& ptr = f.pointer;
        const bool inside = ptr.inside && contains(pr, ptr.x, ptr.y);
        if (inside && ptr.pressed && active_ == Hot::None) {
            active_ = Hot::Play;
        }
        const bool hover = inside && (active_ == Hot::None || active_ == Hot::Play);
        if (active_ == Hot::Play && ptr.released && inside) {
            f.queue.enqueue(CommandId::InspectorPlay);
        }
        const Color fill = hover ? mix(pal.accent, rgb(0xFFFFFF), 0.10F) : pal.accent;
        r.fill_rounded_rect(pr, std::round(p.px(radius::md - 1.0F)), fill);
        const float tx = pr.x + p.px(11.0F);
        const float cy = pr.center_y();
        const float th = p.px(4.6F);
        r.fill_polygon(
            {SDL_FPoint{tx, cy - th}, SDL_FPoint{tx + (1.65F * th), cy}, SDL_FPoint{tx, cy + th}},
            pal.on_accent);
        p.draw(lbl, std::round(tx + p.px(13.0F)), std::round(cy + (0.5F * lbl.cap_height())),
               pal.on_accent);
        right = pr.x - gap;
    }

    // Title and subtitle.
    const float text_r = right - std::round(p.px(space::sm));
    std::string title;
    std::string subtitle;
    if (multi) {
        title = tr_count("inspector.selected", c.selected_count());
        subtitle = format_size(c.selected_bytes());
    } else if (item != nullptr) {
        title = display_name(item->entry.name);
        subtitle = header_subtitle(item->entry, c.result_is_current() ? c.result().get() : nullptr);
    } else {
        title = std::string(tr("inspector.title"));
    }
    const float room = std::max(0.0F, text_r - area.x);
    const bool two = !subtitle.empty();
    const ShapedText t = fitter_.fit(p, type::inspector_title, title, room, Elide::Middle);
    const float base1 = two ? std::round(area.center_y() - p.px(2.5F))
                            : std::round(area.center_y() + (0.5F * t.cap_height()));
    p.draw(t, area.x, base1, pal.text_primary);
    if (two) {
        const ShapedText st = fitter_.fit(p, type::caption, subtitle, room, Elide::End);
        p.draw(st, area.x, std::round(base1 + p.px(16.0F)), pal.text_secondary);
    }
    if (c.locked()) {
        // A quiet accent rule under the header says the Inspector no longer follows the cursor.
        r.fill_rect(RectF{area.x, area.bottom() - std::round(p.px(2.0F)), std::round(p.px(28.0F)),
                          std::round(p.px(2.0F))},
                    pal.accent.with_alpha(0.8F));
    }
}

// ---------------------------------------------------------------------------------------------
// Body
// ---------------------------------------------------------------------------------------------

void InspectorView::draw_body(const InspectorFrame& f, const RectF& area, InspectorOutput& out) {
    const Painter& p = f.painter;
    const Controller& c = f.controller;
    const Status st = c.status();
    const bool right = f.placement == layout::InspectorPlacement::Right;
    const preview::PreviewResult* res = c.result().get();
    text_max_ = 0.0;
    media_ = RectF{};

    // Rebuild the metadata when the content or the language changes.
    const std::string lang = language() == Language::Turkish ? "tr" : "en";
    if (meta_serial_ != c.serial() || meta_language_ != lang) {
        meta_serial_ = c.serial();
        meta_language_ = lang;
        meta_.clear();
        if (st == Status::Multi) {
            meta_.push_back(MetaGroup{
                std::string(tr("meta.group.selection")),
                {MetaRow{std::string(tr("meta.items")), format_count(c.selected_count())},
                 MetaRow{std::string(tr("meta.total_size")), format_size(c.selected_bytes())}}});
        } else if (c.item() != nullptr) {
            meta_ = build_metadata(c.item()->entry, c.result_is_current() ? res : nullptr, f.now);
        }
    }

    if (st == Status::Empty) {
        draw_state(f, area, tr("inspector.empty"), tr("inspector.empty_detail"), GEmpty);
        return;
    }
    if (st == Status::Multi) {
        const RectF top{area.x, area.y, area.w, std::min(area.h, std::round(p.px(170.0F)))};
        draw_state(f, top, tr_count("inspector.selected", c.selected_count()),
                   format_size(c.selected_bytes()), GStack);
        draw_meta(f, RectF{area.x, top.bottom(), area.w, area.bottom() - top.bottom()}, out);
        return;
    }
    const Item* item = c.item();
    const std::string_view mime =
        item != nullptr ? mime::guess_for_entry(item->entry) : std::string_view();
    const bool visual = is_visual_mime(mime);
    const bool video = mime.starts_with("video/");

    // Text and folder take the whole body.
    if (res != nullptr && c.result_is_current()) {
        if (const auto* txt = std::get_if<preview::TextPreview>(&res->content)) {
            draw_text(f, area, *txt);
            return;
        }
        if (const auto* dir = std::get_if<preview::FolderPreview>(&res->content)) {
            draw_folder(f, area, *dir);
            return;
        }
    }

    // Images and videos: the media well, then the metadata beside (Bottom) or below (Right).
    const bool show_media =
        (visual && st != Status::Failed) ||
        (res != nullptr && (std::holds_alternative<preview::ImagePreview>(res->content) ||
                            std::holds_alternative<preview::VideoPreview>(res->content)));
    if (show_media) {
        double aspect = 0.75;
        if (res != nullptr) {
            if (const auto* img = std::get_if<preview::ImagePreview>(&res->content)) {
                const preview::PixelSize d =
                    img->info.display.empty() ? img->image.size() : img->info.display;
                if (!d.empty()) {
                    aspect = static_cast<double>(d.height) / d.width;
                }
            } else if (const auto* vid = std::get_if<preview::VideoPreview>(&res->content)) {
                const preview::PixelSize d =
                    !vid->info.frame.empty() ? vid->info.frame : vid->poster.size();
                aspect = d.empty() ? 0.5625 : static_cast<double>(d.height) / d.width;
            }
        } else if (f.placeholder != nullptr && f.placeholder->width > 0) {
            aspect = static_cast<double>(f.placeholder->height) / f.placeholder->width;
        } else if (video) {
            aspect = 0.5625;
        }
        aspect = std::clamp(aspect, 0.2, 3.0);
        RectF media;
        RectF meta;
        if (right) {
            const auto ideal = static_cast<float>(area.w * aspect);
            const float h = std::clamp(ideal, std::min(p.px(150.0F), area.h),
                                       std::max(p.px(150.0F), area.h * 0.62F));
            media = RectF{area.x, area.y, area.w, std::round(std::min(h, area.h))};
            meta = RectF{area.x, media.bottom(), area.w, area.bottom() - media.bottom()};
        } else {
            const auto ideal = static_cast<float>(area.h / aspect);
            const float w = std::clamp(ideal, std::min(p.px(200.0F), area.w),
                                       std::max(p.px(200.0F), area.w * 0.6F));
            media = RectF{area.x, area.y, std::round(std::min(w, area.w)), area.h};
            meta = RectF{media.right(), area.y, area.right() - media.right(), area.h};
        }
        media_ = media;
        draw_media(f, media, res);
        draw_meta(f, meta, out);
        return;
    }

    // States with the type glyph: unsupported, coming soon, binary, error, loading text/folder.
    const RectF top{area.x, area.y, area.w,
                    std::min(area.h, std::round(p.px(right ? 200.0F : 150.0F)))};
    if (st == Status::Failed) {
        const ErrorCode code = c.error() ? c.error()->code : ErrorCode::Unsupported;
        draw_state(f, top,
                   tr(code == ErrorCode::Unsupported ? "inspector.unsupported" : "inspector.error"),
                   tr(error_detail_key(code)), GAlert);
    } else if (st == Status::Loading) {
        draw_state(f, top, tr("inspector.loading"), "", GEntry);
        animating_ = true;
    } else if (res != nullptr) {
        const auto* ph = std::get_if<preview::PlaceholderPreview>(&res->content);
        const preview::PlaceholderReason reason =
            ph != nullptr ? ph->reason : preview::PlaceholderReason::NoProvider;
        std::string_view title = tr("inspector.unsupported");
        std::string_view detail = tr("inspector.unsupported_detail");
        switch (reason) {
        case preview::PlaceholderReason::ComingSoon:
            title = tr("inspector.coming_soon");
            detail = tr(mime == "application/pdf" ? "inspector.coming_soon_pdf"
                                                  : "inspector.coming_soon_detail");
            break;
        case preview::PlaceholderReason::Binary:
            title = tr("inspector.binary");
            detail = tr("inspector.binary_detail");
            break;
        case preview::PlaceholderReason::KnownFailure:
            title = tr("inspector.error");
            detail = tr("inspector.error.damaged");
            break;
        case preview::PlaceholderReason::NoProvider:
        case preview::PlaceholderReason::NoThumbnailer:
        case preview::PlaceholderReason::NotGenerated: break;
        }
        draw_state(f, top, title, detail, GEntry);
    }
    draw_meta(f, RectF{area.x, top.bottom(), area.w, area.bottom() - top.bottom()}, out);
}

void InspectorView::draw_state(const InspectorFrame& f, const RectF& area, std::string_view title,
                               std::string_view detail, int glyph) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float g = std::round(p.px(glyph == GEntry ? 56.0F : 40.0F));
    const ShapedText t = fitter_.fit(p, type::empty_title, title, area.w - p.px(32.0F), Elide::End);
    const ShapedText d =
        detail.empty() ? ShapedText{}
                       : fitter_.fit(p, type::caption, detail, area.w - p.px(32.0F), Elide::End);
    const float block = g + p.px(16.0F) + p.px(18.0F) + (detail.empty() ? 0.0F : p.px(18.0F));
    float y = std::round(area.center_y() - (0.5F * block));
    const float cx = area.center_x();
    switch (glyph) {
    case GEntry:
        if (const Item* item = f.controller.item(); item != nullptr) {
            draw_entry_glyph(r, pal, item->entry, std::round(cx - (0.5F * g)), y, g, 1.0F,
                             p.scale());
        }
        break;
    case GAlert:
        draw_alert_glyph(r, cx, y + (0.5F * g), 0.5F * g * 0.8F, pal.danger, pal.surface_raised,
                         p.scale());
        break;
    case GStack: {
        const float w = g * 0.72F;
        const float h = g * 0.86F;
        for (int i = 2; i >= 0; --i) {
            const float o = p.px(5.0F) * static_cast<float>(i);
            const RectF pg{std::round(cx - (0.5F * w) + o - p.px(5.0F)), std::round(y + o), w, h};
            r.fill_rounded_rect(pg, p.px(4.0F), over(pal.surface_raised, pal.icon_page));
            r.stroke_rounded_rect(pg, p.px(4.0F), std::max(1.0F, std::round(p.px(1.0F))),
                                  pal.icon_page_border);
        }
        break;
    }
    default: {
        // Empty: a hollow page.
        const RectF pg{std::round(cx - (0.36F * g)), y, std::round(0.72F * g), g};
        r.stroke_rounded_rect(pg, p.px(5.0F), std::max(1.0F, std::round(p.px(1.4F))),
                              pal.text_muted);
        break;
    }
    }
    y += g + p.px(16.0F);
    p.draw(t, std::round(cx - (0.5F * t.width())), std::round(y + t.cap_height()),
           pal.text_primary);
    if (!detail.empty()) {
        y += p.px(22.0F);
        p.draw(d, std::round(cx - (0.5F * d.width())), std::round(y + d.cap_height()),
               pal.text_secondary);
    }
}

// ---------------------------------------------------------------------------------------------
// Image / video
// ---------------------------------------------------------------------------------------------

void InspectorView::draw_skeleton(const InspectorFrame& f, const RectF& area, double aspect) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float pad = std::round(p.px(space::lg));
    const RectF room = inset(area, pad, pad);
    float w = room.w;
    auto h = static_cast<float>(w * aspect);
    if (h > room.h) {
        h = room.h;
        w = static_cast<float>(h / aspect);
    }
    const RectF box = snap(RectF{room.center_x() - (0.5F * w), room.center_y() - (0.5F * h), w, h});
    const float rad = std::round(p.px(radius::sm));
    if (f.placeholder != nullptr && !f.placeholder->tiles.empty()) {
        // The list thumbnail, magnified: a soft preview of what is coming.
        r.draw_image_rounded(f.placeholder->tiles.front().texture, box, rad, 0.85F);
    } else {
        r.fill_rounded_rect(
            box, rad, f.theme == Theme::Dark ? pal.surface_raised.with_alpha(0.0F) : pal.surface);
        r.fill_rounded_rect(box, rad, pal.text_muted.with_alpha(0.10F));
    }
    if (f.animations) {
        // A slow shimmer sweep says "working" without a spinner.
        const double period = 1400.0;
        const auto t = static_cast<float>(std::fmod(f.now_ms, period) / period);
        const float bw = box.w * 0.35F;
        const float x = box.x - bw + ((box.w + bw) * t);
        const RectF band = intersect(RectF{x, box.y, bw, box.h}, box);
        if (band.w > 0.0F) {
            r.fill_rect(band, rgb(0xFFFFFF).with_alpha(f.theme == Theme::Dark ? 0.035F : 0.18F));
        }
        animating_ = true;
    }
}

void InspectorView::draw_play_disc(const InspectorFrame& f, const RectF& area) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float rad = std::round(p.px(26.0F));
    const float cx = std::round(area.center_x());
    const float cy = std::round(area.center_y());
    const float dx = ptr.x - cx;
    const float dy = ptr.y - cy;
    const bool inside = ptr.inside && ((dx * dx) + (dy * dy)) <= (rad * rad);
    if (inside && ptr.pressed && active_ == Hot::None) {
        active_ = Hot::Play;
    }
    const bool hover = inside && (active_ == Hot::None || active_ == Hot::Play);
    if (active_ == Hot::Play && ptr.released && inside) {
        f.queue.enqueue(CommandId::InspectorPlay);
    }
    r.fill_circle(cx, cy + p.px(1.0F), rad + p.px(2.0F), rgb(0x000000).with_alpha(0.18F));
    r.fill_circle(cx, cy, rad,
                  hover ? pal.accent.with_alpha(0.92F) : rgb(0x101114).with_alpha(0.58F));
    r.stroke_rounded_rect(RectF{cx - rad, cy - rad, 2.0F * rad, 2.0F * rad}, rad,
                          std::max(1.0F, std::round(p.px(1.5F))), rgb(0xFFFFFF).with_alpha(0.85F));
    const float th = p.px(10.0F);
    const float ox = p.px(2.5F); // optical centre of the triangle
    r.fill_polygon({SDL_FPoint{cx - (0.62F * th) + ox, cy - th},
                    SDL_FPoint{cx + (1.05F * th) + ox, cy},
                    SDL_FPoint{cx - (0.62F * th) + ox, cy + th}},
                   rgb(0xFFFFFF));
}

void InspectorView::draw_media(const InspectorFrame& f, const RectF& area,
                               const preview::PreviewResult* res) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const Controller& c = f.controller;
    r.fill_rect(area, well_color(pal, f.theme));

    const preview::ImageInfo* info = nullptr;
    const preview::VideoPreview* vid = nullptr;
    if (res != nullptr) {
        if (const auto* img = std::get_if<preview::ImagePreview>(&res->content)) {
            info = &img->info;
        }
        vid = std::get_if<preview::VideoPreview>(&res->content);
    }
    // Texture of the current result (uploaded at frame start within the budget).
    const TextureCache::Texture* tex = nullptr;
    if (const std::optional<preview::PreviewKey>& rk = c.result_key(); res != nullptr && rk) {
        f.textures.request(*rk, c.result(), UploadClass::Inspector);
        tex = f.textures.get(*rk);
        if (tex != nullptr) {
            if (!shown_key_ || !(*shown_key_ == *c.result_key())) {
                // A first image fades in; a sharper version of the same item replaces in place.
                if (!shown_key_) {
                    fade_start_ms_ = f.now_ms;
                }
                shown_key_ = *c.result_key();
            }
        } else if (shown_key_) {
            tex = f.textures.get(*shown_key_); // keep the previous texture until the new lands
        }
    }

    const float pad = std::round(p.px(space::md));
    const RectF view = inset(area, pad, pad);
    if (vid != nullptr) {
        if (tex != nullptr) {
            // Poster: fit (small thumbnailer posters may grow up to 2x), never zoomed.
            const float s = std::min({view.w / static_cast<float>(tex->width),
                                      view.h / static_cast<float>(tex->height), 2.0F});
            const float w = std::round(static_cast<float>(tex->width) * s);
            const float h = std::round(static_cast<float>(tex->height) * s);
            const RectF dst{std::round(view.center_x() - (0.5F * w)),
                            std::round(view.center_y() - (0.5F * h)), w, h};
            float a = 1.0F;
            if (f.animations && fade_start_ms_ >= 0.0 && f.now_ms - fade_start_ms_ < kFadeMs) {
                a = static_cast<float>((f.now_ms - fade_start_ms_) / kFadeMs);
                animating_ = true;
            }
            r.fill_rounded_rect(dst, std::round(p.px(radius::sm)), rgb(0x000000));
            r.draw_image_rounded(tex->tiles.empty() ? nullptr : tex->tiles.front().texture, dst,
                                 std::round(p.px(radius::sm)), a);
        } else {
            // No poster: the type glyph and a quiet line.
            const float g = std::round(p.px(56.0F));
            if (const Item* item = c.item(); item != nullptr) {
                draw_entry_glyph(r, pal, item->entry, std::round(view.center_x() - (0.5F * g)),
                                 std::round(view.center_y() - (0.5F * g) - p.px(34.0F)), g, 1.0F,
                                 p.scale());
            }
            const ShapedText t =
                fitter_.fit(p, type::caption, tr("inspector.no_poster"), view.w, Elide::End);
            p.draw(t, std::round(view.center_x() - (0.5F * t.width())),
                   std::round(view.bottom() - p.px(14.0F)), pal.text_secondary);
        }
        if (vid->info.source == preview::MetadataSource::None) {
            const ShapedText t = fitter_.fit(p, type::caption, tr("inspector.video_no_metadata"),
                                             view.w, Elide::End);
            const float tw = t.width() + p.px(16.0F);
            const RectF chip{std::round(view.center_x() - (0.5F * tw)), std::round(view.y),
                             std::round(tw), std::round(p.px(22.0F))};
            r.fill_rounded_rect(chip, 0.5F * chip.h, pal.surface_raised.with_alpha(0.92F));
            p.draw(t, chip.x + p.px(8.0F), std::round(chip.center_y() + (0.5F * t.cap_height())),
                   pal.text_secondary);
        }
        draw_play_disc(f, view);
        return;
    }
    if (tex == nullptr) {
        double aspect = 0.75;
        if (info != nullptr && !info->display.empty()) {
            aspect = static_cast<double>(info->display.height) / info->display.width;
        } else if (f.placeholder != nullptr && f.placeholder->width > 0) {
            aspect = static_cast<double>(f.placeholder->height) / f.placeholder->width;
        }
        draw_skeleton(f, area, std::clamp(aspect, 0.2, 5.0));
        // Fit target for the first decode.
        const DecodeTarget dt = bucket_target(view.w, view.h);
        fit_target_ = preview::PixelSize{dt.width, dt.height};
        return;
    }
    draw_image(f, view, tex, info);
}

void InspectorView::draw_image(const InspectorFrame& f, const RectF& view,
                               const TextureCache::Texture* tex, const preview::ImageInfo* info) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const PointerInput& ptr = f.pointer;
    const double iw = info != nullptr && !info->display.empty()
                          ? static_cast<double>(info->display.width)
                          : static_cast<double>(tex->width);
    const double ih = info != nullptr && !info->display.empty()
                          ? static_cast<double>(info->display.height)
                          : static_cast<double>(tex->height);
    if (viewport_.image_width() != iw || viewport_.image_height() != ih) {
        viewport_.set_image(iw, ih);
    }
    viewport_.set_view(view.w, view.h);
    if (viewport_.animate(f.dt_ms, f.animations)) {
        animating_ = true;
    }

    // Pointer: wheel zoom about the pointer, drag to pan, double-click toggles fit / 1:1.
    const bool over = ptr.inside && contains(view, ptr.x, ptr.y);
    const Vec2 local{static_cast<double>(ptr.x - view.x), static_cast<double>(ptr.y - view.y)};
    if (over && active_ == Hot::None && ptr.wheel_y != 0.0F) {
        viewport_.zoom_smooth(std::pow(kWheelZoomStep, static_cast<double>(ptr.wheel_y)), local);
        animating_ = true;
    }
    if (over && ptr.pressed && active_ == Hot::None) {
        if (ptr.clicks >= 2) {
            viewport_.toggle_fit_1to1(local);
        } else {
            active_ = Hot::Image;
            last_x_ = ptr.x;
            last_y_ = ptr.y;
        }
    }
    if (active_ == Hot::Image && ptr.down) {
        viewport_.pan(static_cast<double>(ptr.x - last_x_), static_cast<double>(ptr.y - last_y_));
        last_x_ = ptr.x;
        last_y_ = ptr.y;
        animating_ = true;
    }

    // Decode targets: fit at the view size; full resolution once the zoom needs more pixels
    // than the decoded buffer has.
    const DecodeTarget dt = bucket_target(view.w, view.h);
    fit_target_ = preview::PixelSize{dt.width, dt.height};
    const double needed = viewport_.zoom() * iw;
    full_res_ = full_res_ || (viewport_.mode() != ZoomMode::Fit && needed > tex->width * 1.05);
    if (viewport_.mode() == ZoomMode::Fit) {
        full_res_ = false;
    }

    const Box ib = viewport_.image_rect();
    const RectF dst{view.x + static_cast<float>(ib.x), view.y + static_cast<float>(ib.y),
                    static_cast<float>(ib.w), static_cast<float>(ib.h)};
    const RectF visible = intersect(dst, view);
    const std::optional<RectF> saved = r.clip();
    r.set_clip(saved ? intersect(*saved, view) : view);
    float a = 1.0F;
    if (f.animations && fade_start_ms_ >= 0.0 && f.now_ms - fade_start_ms_ < kFadeMs) {
        a = static_cast<float>((f.now_ms - fade_start_ms_) / kFadeMs);
        animating_ = true;
    }
    const bool alpha = info != nullptr ? info->has_alpha : false;
    if (alpha && visible.w > 0.0F) {
        // Neutral checkerboard (both themes) under transparency, anchored to the image.
        const float cell = std::round(p.px(kCheckerCell));
        const float ox = dst.x + (std::floor((visible.x - dst.x) / cell) * cell);
        const float oy = dst.y + (std::floor((visible.y - dst.y) / cell) * cell);
        r.set_clip(visible);
        checker(r, RectF{ox, oy, visible.right() - ox, visible.bottom() - oy}, cell, f.theme);
        r.set_clip(saved ? intersect(*saved, view) : view);
    }
    if (a < 1.0F && f.placeholder != nullptr && !f.placeholder->tiles.empty()) {
        TextureCache::draw(r, *f.placeholder, dst, 1.0F - a, false);
    }
    const bool nearest = viewport_.zoom() >= 2.0;
    TextureCache::draw(r, *tex, dst, a, nearest);

    // Pixel grid at >= 4:1.
    if (f.state.show_grid && viewport_.grid_visible() && visible.w > 0.0F) {
        const auto z = static_cast<float>(viewport_.zoom());
        const Color line = rgb(0x808080).with_alpha(0.35F);
        const float x0 = dst.x + (std::ceil((visible.x - dst.x) / z) * z);
        for (float x = x0; x < visible.right(); x += z) {
            r.fill_rect(RectF{std::round(x), visible.y, 1.0F, visible.h}, line);
        }
        const float y0 = dst.y + (std::ceil((visible.y - dst.y) / z) * z);
        for (float y = y0; y < visible.bottom(); y += z) {
            r.fill_rect(RectF{visible.x, std::round(y), visible.w, 1.0F}, line);
        }
    }
    r.set_clip(saved);

    if (f.state.show_minimap && viewport_.pannable()) {
        draw_minimap(f, view, *tex);
    }
    // The zoom toolbar shows while the pointer is over the image, while the Inspector has the
    // keyboard, and whenever the image is not in fit (so the way back is always visible).
    if (over || f.keyboard || viewport_.mode() != ZoomMode::Fit || active_ == Hot::Image) {
        draw_toolbar(f, view);
    }
}

void InspectorView::draw_minimap(const InspectorFrame& f, const RectF& view,
                                 const TextureCache::Texture& tex) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float m = std::round(p.px(10.0F));
    const float mw = std::round(std::min(p.px(120.0F), view.w * 0.38F));
    const float mh = std::round(std::min(p.px(90.0F), view.h * 0.38F));
    const Box frame = minimap_frame(viewport_.image_width(), viewport_.image_height(),
                                    view.right() - m - mw, view.y + m, mw, mh);
    const RectF fr{static_cast<float>(frame.x), static_cast<float>(frame.y),
                   static_cast<float>(frame.w), static_cast<float>(frame.h)};
    const float pad = std::round(p.px(3.0F));
    const RectF plate{fr.x - pad, fr.y - pad, fr.w + (2.0F * pad), fr.h + (2.0F * pad)};
    r.drop_shadow(plate, p.px(5.0F), p.px(10.0F), p.px(2.0F), pal.shadow);
    r.fill_rounded_rect(plate, p.px(5.0F), pal.surface_raised.with_alpha(0.96F));
    TextureCache::draw(r, tex, fr, 1.0F, false);
    const Box vb = minimap_visible(viewport_, frame);
    const RectF vis{static_cast<float>(vb.x), static_cast<float>(vb.y), static_cast<float>(vb.w),
                    static_cast<float>(vb.h)};
    // Dim outside the visible rectangle, outline it in the accent.
    const Color dim = rgb(0x000000).with_alpha(0.38F);
    r.fill_rect(RectF{fr.x, fr.y, fr.w, vis.y - fr.y}, dim);
    r.fill_rect(RectF{fr.x, vis.bottom(), fr.w, fr.bottom() - vis.bottom()}, dim);
    r.fill_rect(RectF{fr.x, vis.y, vis.x - fr.x, vis.h}, dim);
    r.fill_rect(RectF{vis.right(), vis.y, fr.right() - vis.right(), vis.h}, dim);
    r.stroke_rounded_rect(snap(vis), p.px(2.0F), std::max(1.0F, std::round(p.px(1.5F))),
                          pal.accent);

    const bool inside = ptr.inside && contains(plate, ptr.x, ptr.y);
    if (inside && ptr.pressed && (active_ == Hot::None || active_ == Hot::Image)) {
        active_ = Hot::Minimap;
    }
    if (active_ == Hot::Minimap && ptr.down) {
        viewport_.center_on(minimap_to_image(viewport_, frame, Vec2{ptr.x, ptr.y}));
        animating_ = true;
    }
}

void InspectorView::draw_toolbar(const InspectorFrame& f, const RectF& view) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float b = std::round(p.px(26.0F));
    const float gap = std::round(p.px(2.0F));
    const ShapedText pct =
        p.shape(type::status,
                std::to_string(static_cast<int>(std::lround(viewport_.zoom() * 100.0))) + "%");
    const float label_w =
        std::round(std::max(pct.width(), p.shape(type::status, "1600%").width()) + p.px(12.0F));
    const float w = (4.0F * b) + (3.0F * gap) + label_w + (2.0F * std::round(p.px(4.0F))) + gap;
    if (w > view.w - p.px(16.0F)) {
        return; // too narrow: header commands and the palette still work
    }
    const float h = b + (2.0F * std::round(p.px(3.0F)));
    const RectF bar =
        snap(RectF{view.center_x() - (0.5F * w), view.bottom() - h - p.px(10.0F), w, h});
    r.drop_shadow(bar, 0.5F * h, p.px(12.0F), p.px(2.0F), pal.shadow);
    r.fill_rounded_rect(bar, 0.5F * h, pal.surface_raised.with_alpha(0.97F));
    r.stroke_rounded_rect(bar, 0.5F * h, std::max(1.0F, std::round(p.px(1.0F))), pal.border);
    float x = bar.x + std::round(p.px(4.0F));
    const float y = bar.y + std::round(p.px(3.0F));
    const bool fit = viewport_.mode() == ZoomMode::Fit;
    const bool one = viewport_.mode() == ZoomMode::OneToOne;
    if (icon_button(f, RectF{x, y, b, b}, Hot::Fit, GFit, fit)) {
        f.queue.enqueue(CommandId::InspectorZoomFit);
    }
    x += b + gap;
    if (icon_button(f, RectF{x, y, b, b}, Hot::OneToOne, GOne, one)) {
        f.queue.enqueue(CommandId::InspectorZoom1to1);
    }
    x += b + gap;
    p.draw(pct, std::round(x + (0.5F * (label_w - pct.width()))),
           std::round(bar.center_y() + (0.5F * pct.cap_height())), pal.text_secondary);
    x += label_w + gap;
    if (icon_button(f, RectF{x, y, b, b}, Hot::Grid, GGrid, f.state.show_grid)) {
        f.queue.enqueue(CommandId::ToggleInspectorGrid);
    }
    x += b + gap;
    if (icon_button(f, RectF{x, y, b, b}, Hot::Minimap2, GMinimap, f.state.show_minimap)) {
        f.queue.enqueue(CommandId::ToggleInspectorMinimap);
    }
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

void InspectorView::draw_scrollbar(const InspectorFrame& f, const RectF& track, double offset,
                                   double max, double view) {
    if (max <= 0.0 || track.h <= 0.0F) {
        return;
    }
    const Painter& p = f.painter;
    const double total = max + view;
    const float h = std::max(p.px(24.0F), static_cast<float>(track.h * (view / total)));
    const float y = track.y + static_cast<float>((track.h - h) * (offset / max));
    const float w = std::round(p.px(metrics::scrollbar_width));
    p.renderer().fill_rounded_rect(RectF{track.right() - w - p.px(3.0F), y, w, h}, 0.5F * w,
                                   f.palette.scrollbar);
}

void InspectorView::draw_text(const InspectorFrame& f, const RectF& area,
                              const preview::TextPreview& text) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float footer = text.truncated ? std::round(p.px(28.0F)) : 0.0F;
    const RectF view{area.x, area.y, area.w, area.h - footer};
    const ShapedText digit = p.shape(type::code, "0");
    const float adv = std::max(1.0F, digit.width());
    const float line_h = std::round(p.px(type::code.size * 1.5F));
    const std::string last_line = std::to_string(std::max<std::uint32_t>(text.line_count, 1));
    const float gutter = std::round(
        (static_cast<float>(std::max<std::size_t>(last_line.size(), 2)) * adv) + p.px(22.0F));
    const float pad = std::round(p.px(space::md));
    const float text_x = view.x + gutter + pad;
    const float text_w = view.right() - text_x - pad;
    const auto cols = static_cast<std::uint32_t>(std::max(8.0F, std::floor(text_w / adv)));
    if (!wrap_.matches(&text, cols)) {
        wrap_.rebuild(&text, cols);
    }
    const double top_pad = p.px(space::sm);
    const std::size_t lines = wrap_.visual_lines();
    text_max_ = max_scroll(view.h - (2.0 * top_pad), line_h, lines);
    text_page_ = view.h;
    if (ptr.inside && contains(view, ptr.x, ptr.y) && ptr.wheel_y != 0.0F && active_ == Hot::None) {
        text_scroll_ -= static_cast<double>(ptr.wheel_y) * 3.0 * line_h;
    }
    text_scroll_ = std::clamp(text_scroll_, 0.0, text_max_);

    // Gutter: a quieter band with line numbers.
    r.fill_rect(RectF{view.x, view.y, gutter, view.h}, pal.text_muted.with_alpha(0.055F));
    r.fill_rect(RectF{view.x + gutter, view.y, std::max(1.0F, std::round(p.px(1.0F))), view.h},
                pal.border.with_alpha(0.6F));
    const LineRange range = visible_lines(text_scroll_, view.h, line_h, lines);
    const float base_off = std::round((0.5F * line_h) + (0.5F * digit.cap_height()));
    for (std::size_t k = range.first; k < range.last; ++k) {
        const VisualLine vl = wrap_.line(k);
        const float y =
            view.y + static_cast<float>(top_pad + (static_cast<double>(k) * line_h) - text_scroll_);
        const float base = std::round(y + base_off);
        if (vl.first_of_line) {
            const ShapedText num = p.shape(type::code_gutter, std::to_string(vl.line + 1));
            p.draw(num, std::round(view.x + gutter - p.px(10.0F) - num.width()), base,
                   pal.text_muted);
        }
        const std::string_view seg =
            text.row_text(vl.row).substr(vl.byte_begin, vl.byte_end - vl.byte_begin);
        if (seg.empty()) {
            continue;
        }
        const ShapedText t = p.shape(type::code, display_text(seg, vl.cell_begin));
        p.draw(t, text_x, base, pal.text_primary);
    }
    draw_scrollbar(f, view, text_scroll_, text_max_, view.h);

    if (footer > 0.0F) {
        const RectF bar{area.x, view.bottom(), area.w, footer};
        r.fill_rect(bar, pal.text_muted.with_alpha(0.07F));
        r.fill_rect(RectF{bar.x, bar.y, bar.w, std::max(1.0F, std::round(p.px(1.0F)))}, pal.border);
        const std::string msg =
            substitute(tr("inspector.text_truncated"), "size", format_size(text.bytes_read));
        const ShapedText t = fitter_.fit(p, type::caption, msg, bar.w - (2.0F * pad), Elide::End);
        p.draw(t, bar.x + pad, std::round(bar.center_y() + (0.5F * t.cap_height())),
               pal.text_secondary);
    }
}

// ---------------------------------------------------------------------------------------------
// Folder
// ---------------------------------------------------------------------------------------------

void InspectorView::draw_folder(const InspectorFrame& f, const RectF& area,
                                const preview::FolderPreview& folder) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float pad = std::round(p.px(space::md));
    // Summary line.
    std::string summary = tr_count("inspector.folders", folder.folders) + "  ·  " +
                          tr_count("inspector.files", folder.files);
    if (folder.hidden > 0) {
        summary += "  ·  " + tr_count("inspector.hidden", folder.hidden);
    }
    const float sum_h = std::round(p.px(32.0F));
    const ShapedText st = fitter_.fit(p, type::caption, summary, area.w - (2.0F * pad), Elide::End);
    p.draw(st, area.x + pad, std::round(area.y + (0.5F * sum_h) + (0.5F * st.cap_height())),
           pal.text_secondary);
    const RectF view{area.x, area.y + sum_h, area.w, area.h - sum_h};
    if (folder.entries.empty()) {
        draw_state(
            f, RectF{view.x, view.y, view.w, std::min(view.h, p.px(150.0F))},
            tr(folder.hidden > 0 ? "inspector.folder_only_hidden" : "inspector.folder_empty"), "",
            GEmpty);
        return;
    }
    const float row_h = std::round(p.px(kFolderRowH));
    const std::size_t count = folder.entries.size() + (folder.truncated ? 1 : 0);
    text_max_ = max_scroll(view.h, row_h, count);
    text_page_ = view.h;
    if (ptr.inside && contains(view, ptr.x, ptr.y) && ptr.wheel_y != 0.0F && active_ == Hot::None) {
        text_scroll_ -= static_cast<double>(ptr.wheel_y) * 3.0 * row_h;
    }
    text_scroll_ = std::clamp(text_scroll_, 0.0, text_max_);
    const std::optional<RectF> saved = r.clip();
    r.set_clip(saved ? intersect(*saved, view) : view);
    const LineRange range = visible_lines(text_scroll_, view.h, row_h, count);
    const float glyph = std::round(p.px(metrics::icon_size));
    const float rad = std::round(p.px(radius::sm));
    for (std::size_t i = range.first; i < range.last; ++i) {
        const float y =
            view.y + static_cast<float>((static_cast<double>(i) * row_h) - text_scroll_);
        const RectF row{view.x + std::round(p.px(metrics::row_inset)), y,
                        view.w - (2.0F * std::round(p.px(metrics::row_inset))), row_h};
        if (i % 2 == 1) {
            r.fill_rounded_rect(row, rad, pal.row_alt);
        }
        const float mid = std::round(y + (0.5F * row_h));
        if (i >= folder.entries.size()) {
            const std::string more =
                tr_count("inspector.more", folder.total - folder.entries.size());
            const ShapedText t = p.shape(type::row, more);
            p.draw(t, row.x + pad + glyph + p.px(8.0F), std::round(mid + (0.5F * t.cap_height())),
                   pal.text_muted);
            continue;
        }
        const fs::DirEntry& e = folder.entries[i];
        const float dim = e.hidden ? 0.6F : 1.0F;
        draw_entry_glyph(r, pal, e, row.x + pad - p.px(4.0F), std::round(mid - (0.5F * glyph)),
                         glyph, dim, p.scale());
        const float tx = row.x + pad - p.px(4.0F) + glyph + p.px(8.0F);
        std::string right_text;
        if (!fs::is_directory_like(e) && !e.metadata_error) {
            right_text = format_size(e.size);
        }
        const ShapedText rt = p.shape(type::row_numeric, right_text);
        const float rx = row.right() - pad - rt.width();
        const ShapedText nt =
            fitter_.fit(p, type::row, display_name(e.name), std::max(0.0F, rx - tx - p.px(8.0F)),
                        fs::is_directory_like(e) ? Elide::End : Elide::Middle);
        const float base = std::round(mid + (0.5F * nt.cap_height()));
        p.draw(nt, tx, base, e.hidden ? pal.text_secondary : pal.text_primary);
        if (!right_text.empty()) {
            p.draw(rt, rx, base, pal.text_secondary);
        }
    }
    r.set_clip(saved);
    draw_scrollbar(f, view, text_scroll_, text_max_, view.h);
}

// ---------------------------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------------------------

void InspectorView::draw_meta(const InspectorFrame& f, const RectF& area, InspectorOutput& out) {
    if (area.w <= 0.0F || area.h <= 0.0F || meta_.empty()) {
        meta_max_ = 0.0;
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float pad = std::round(p.px(space::md));
    const float row_h = std::round(p.px(kMetaRowH));
    const float group_h = std::round(p.px(28.0F));
    // Content height.
    float total = std::round(p.px(space::xs));
    for (const MetaGroup& g : meta_) {
        total += group_h + (static_cast<float>(g.rows.size()) * row_h);
    }
    total += pad;
    meta_max_ = std::max(0.0, static_cast<double>(total - area.h));
    if (ptr.inside && contains(area, ptr.x, ptr.y) && ptr.wheel_y != 0.0F && active_ == Hot::None) {
        meta_scroll_ -= static_cast<double>(ptr.wheel_y) * 3.0 * row_h;
    }
    meta_scroll_ = std::clamp(meta_scroll_, 0.0, meta_max_);

    // Key column: the widest key, capped at 42% of the panel.
    float key_w = 0.0F;
    for (const MetaGroup& g : meta_) {
        for (const MetaRow& row : g.rows) {
            key_w = std::max(key_w, p.shape(type::meta_key, row.label).width());
        }
    }
    const float inner = area.w - (2.0F * pad);
    key_w = std::min(std::ceil(key_w), std::round(inner * 0.42F));
    const float gap = std::round(p.px(space::md));
    const std::optional<RectF> saved = r.clip();
    r.set_clip(saved ? intersect(*saved, area) : area);
    float y = area.y + std::round(p.px(space::xs)) - static_cast<float>(meta_scroll_);
    const float rad = std::round(p.px(radius::sm));
    for (const MetaGroup& g : meta_) {
        const ShapedText gt = p.shape(type::meta_group, g.title);
        p.draw(gt, area.x + pad, std::round(y + group_h - p.px(9.0F)), pal.text_muted);
        y += group_h;
        for (const MetaRow& row : g.rows) {
            const RectF rr{area.x + std::round(0.5F * pad), y, area.w - pad, row_h};
            if (y + row_h >= area.y && y <= area.bottom()) {
                const bool hover = ptr.inside && contains(rr, ptr.x, ptr.y) &&
                                   contains(area, ptr.x, ptr.y) &&
                                   (active_ == Hot::None || active_ == Hot::MetaRow);
                if (hover) {
                    r.fill_rounded_rect(rr, rad, pal.control_hover);
                    if (ptr.pressed && active_ == Hot::None) {
                        active_ = Hot::MetaRow;
                    }
                    if (active_ == Hot::MetaRow && ptr.released) {
                        out.copy = row.value;
                    }
                }
                const ShapedText kt = fitter_.fit(p, type::meta_key, row.label, key_w, Elide::End);
                const float base = std::round(y + (0.5F * row_h) + (0.5F * kt.cap_height()));
                p.draw(kt, area.x + pad, base, pal.text_secondary);
                const float vx = area.x + pad + key_w + gap;
                const ShapedText vt =
                    fitter_.fit(p, type::meta_value, row.value,
                                std::max(0.0F, area.right() - pad - vx), Elide::End);
                p.draw(vt, vx, base, pal.text_primary);
            }
            y += row_h;
        }
    }
    r.set_clip(saved);
    draw_scrollbar(f, area, meta_scroll_, meta_max_, area.h);
}

} // namespace navigator::ui::inspector
