#pragma once
// Pure input-source arbitration: no ImGui, Win32, clock, or application state.
#include <array>
#include <cmath>
#include <cfloat>

namespace ui { namespace hints {
enum class Source { Gamepad = 0, Keyboard = 1, Mouse = 2 };
class SourceTracker {
    std::array<bool, 1024> held_{};
    bool mouseValid_ = false;
    float anchorX_ = 0, anchorY_ = 0, lastX_ = 0, lastY_ = 0;
    void Select(Source next) {
        source = next;
        // Noise accumulated before a keyboard/pad action must not steal it back.
        if (next != Source::Mouse && mouseValid_) { anchorX_ = lastX_; anchorY_ = lastY_; }
    }
public:
    Source source = Source::Keyboard;
    void Key(Source device, int key, bool down, float analog = 1.0f) {
        if (key < 0 || key >= static_cast<int>(held_.size())) return;
        const bool active = down && (device != Source::Gamepad || analog >= 0.20f);
        if (active && !held_[key]) Select(device);
        held_[key] = active; // releases (including synthetic releases) never select a source
    }
    void Character() { Select(Source::Keyboard); }
    void MouseButton(bool down) { if (down) Select(Source::Mouse); }
    void Wheel(float x, float y) { if (x != 0 || y != 0) Select(Source::Mouse); }
    void MousePosition(float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) >= FLT_MAX / 2 || std::fabs(y) >= FLT_MAX / 2) {
            mouseValid_ = false;
            return;
        }
        lastX_ = x; lastY_ = y;
        if (!mouseValid_) { anchorX_ = x; anchorY_ = y; mouseValid_ = true; return; }
        const float dx = x - anchorX_, dy = y - anchorY_;
        if (dx * dx + dy * dy >= 36.0f) {
            anchorX_ = x; anchorY_ = y;
            Select(Source::Mouse);
        }
    }
};
}} // namespace ui::hints
