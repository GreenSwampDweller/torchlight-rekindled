// What a mouse and a PC screen's logic need of a `LayoutScreen`: the window under the
// pointer, a window's `onClick`, a swapped picture and a bar's fill.
#include "render/layout_screen.h"

#include <OgrePanelOverlayElement.h>

#include <algorithm>

namespace tl::render {

std::string LayoutScreen::window_at(float x, float y) const {
    if (!built_ || !visible_) return {};
    for (size_t i = nodes_.size(); i > 0; --i) {
        const Node& node = nodes_[i - 1];
        if (node.widget.empty() || (node.panel == nullptr && node.text == nullptr)) continue;
        if (is_hidden(static_cast<int>(i - 1))) continue;
        const auto found = pc_windows_.find(node.widget);
        if (found != pc_windows_.end() && found->second.pass_through) continue;
        if (x < node.left || y < node.top) continue;
        if (x > node.left + node.width || y > node.top + node.height) continue;
        return node.widget;
    }
    return {};
}

bool LayoutScreen::window_contains(const std::string& widget, float x, float y) const {
    const int index = find_widget(widget);
    if (index < 0) return false;
    const Node& node = nodes_[static_cast<size_t>(index)];
    return x >= node.left && y >= node.top && x <= node.left + node.width &&
           y <= node.top + node.height;
}

bool LayoutScreen::window_box(const std::string& widget, float& left, float& top, float& width,
                              float& height) const {
    const int index = find_widget(widget);
    if (index < 0) return false;
    const Node& node = nodes_[static_cast<size_t>(index)];
    left = node.left;
    top = node.top;
    width = node.width;
    height = node.height;
    return true;
}

bool LayoutScreen::art(const std::string& name, Ogre::MaterialPtr& material,
                       std::array<float, 4>& uv) const {
    const Sprite* found = sprite(name);
    if (found == nullptr || !found->texture || found->rect == nullptr) return false;
    const float wide = static_cast<float>(found->texture->getWidth());
    const float tall = static_cast<float>(found->texture->getHeight());
    material = found->material;
    uv = {static_cast<float>(found->rect->x) / wide, static_cast<float>(found->rect->y) / tall,
          static_cast<float>(found->rect->x + found->rect->width) / wide,
          static_cast<float>(found->rect->y + found->rect->height) / tall};
    return true;
}

void LayoutScreen::set_relative(const std::string& widget, float x, float y) {
    const int index = find_widget(widget);
    if (index < 0) return;
    nodes_[static_cast<size_t>(index)].scale_x = x;
    nodes_[static_cast<size_t>(index)].scale_y = y;
}

std::string LayoutScreen::on_click(const std::string& widget) const {
    const auto found = pc_windows_.find(widget);
    return found == pc_windows_.end() ? std::string() : found->second.on_click;
}

std::string LayoutScreen::image(const std::string& widget) const {
    const int index = find_widget(widget);
    return index < 0 ? std::string() : nodes_[static_cast<size_t>(index)].image;
}

void LayoutScreen::set_image(const std::string& widget, const std::string& image) {
    const int index = find_widget(widget);
    if (index < 0) return;
    Node& node = nodes_[static_cast<size_t>(index)];
    if (node.image == image) return;
    node.image = image;
    paint(node, image);
}

void LayoutScreen::set_fill(const std::string& widget, float amount, bool upward) {
    const int index = find_widget(widget);
    if (index < 0) return;
    Node& node = nodes_[static_cast<size_t>(index)];
    if (node.panel == nullptr) return;
    const Sprite* art = node.image.empty() ? nullptr : sprite(node.image);
    if (art == nullptr || !art->texture) return;
    const float part = std::min(1.0f, std::max(0.0f, amount));
    const float tw = static_cast<float>(art->texture->getWidth());
    const float th = static_cast<float>(art->texture->getHeight());
    const float u0 = static_cast<float>(art->rect->x) / tw;
    const float v0 = static_cast<float>(art->rect->y) / th;
    const float u1 = static_cast<float>(art->rect->x + art->rect->width) / tw;
    const float v1 = static_cast<float>(art->rect->y + art->rect->height) / th;
    if (upward) {
        // The bottom `part` of the picture, standing on the window's bottom edge.
        node.panel->setPosition(node.left, node.top + node.height * (1.0f - part));
        node.panel->setDimensions(node.width, node.height * part);
        node.panel->setUV(u0, v1 - (v1 - v0) * part, u1, v1);
    } else {
        node.panel->setPosition(node.left, node.top);
        node.panel->setDimensions(node.width * part, node.height);
        node.panel->setUV(u0, v0, u0 + (u1 - u0) * part, v1);
    }
}

}  // namespace tl::render
