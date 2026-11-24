// src/ui/ops/sdl_clipboard.cpp — see sdl_clipboard.hpp.
#include "ops/sdl_clipboard.hpp"

#include <SDL3/SDL.h>

namespace navigator::ui {

struct SdlClipboard::Offer {
    SdlClipboard* owner = nullptr;
    std::map<std::string, std::string> data;
    std::vector<std::string> mimes; // stable storage for the C strings
};

namespace {

const void* offer_data(void* userdata, const char* mime_type, std::size_t* size) {
    auto* offer = static_cast<SdlClipboard::Offer*>(userdata);
    if (offer == nullptr || mime_type == nullptr) {
        *size = 0;
        return nullptr;
    }
    const auto it = offer->data.find(mime_type);
    if (it == offer->data.end()) {
        *size = 0;
        return nullptr;
    }
    *size = it->second.size();
    return it->second.data();
}

void offer_cleanup(void* userdata) {
    auto* offer = static_cast<SdlClipboard::Offer*>(userdata);
    if (offer == nullptr) {
        return;
    }
    if (offer->owner != nullptr) {
        offer->owner->released(offer);
    }
    delete offer;
}

} // namespace

SdlClipboard::~SdlClipboard() {
    if (current_ != nullptr) {
        current_->owner = nullptr; // SDL may still call back after us: never into a dead owner
    }
}

void SdlClipboard::released(Offer* offer) noexcept {
    if (offer == current_) {
        current_ = nullptr;
    }
}

void SdlClipboard::offer(std::vector<std::pair<std::string, std::string>> formats) {
    auto* o = new Offer();
    o->owner = this;
    for (auto& [mime, bytes] : formats) {
        o->mimes.push_back(mime);
        o->data[mime] = std::move(bytes);
    }
    std::vector<const char*> ptrs;
    ptrs.reserve(o->mimes.size());
    for (const std::string& m : o->mimes) {
        ptrs.push_back(m.c_str());
    }
    // SDL cleans up the previous offer (calls its cleanup) before installing this one.
    if (!SDL_SetClipboardData(offer_data, offer_cleanup, o, ptrs.data(), ptrs.size())) {
        SDL_Log("Navigator: clipboard unavailable: %s", SDL_GetError());
        delete o;
        current_ = nullptr;
        return;
    }
    current_ = o;
}

std::optional<std::string> SdlClipboard::read(std::string_view mime) {
    const std::string m(mime);
    if (!SDL_HasClipboardData(m.c_str())) {
        return std::nullopt;
    }
    std::size_t size = 0;
    void* data = SDL_GetClipboardData(m.c_str(), &size);
    if (data == nullptr) {
        return std::nullopt;
    }
    std::string out(static_cast<const char*>(data), size);
    SDL_free(data);
    return out;
}

void SdlClipboard::clear_own() {
    if (current_ != nullptr) {
        SDL_ClearClipboardData(); // runs the cleanup callback, which releases current_
        current_ = nullptr;
    }
}

} // namespace navigator::ui
