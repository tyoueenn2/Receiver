#include "preview_window.hpp"
#include "receiver/color.hpp"
#include <algorithm>
#include <cstdio>
namespace receiver::gui {
bool draw_preview_image(const PreviewImage& image, const Preview& picture, const Settings& settings,
                        const Stats& stats, const std::vector<std::string>& names, bool simulated,
                        ImVec2 available, float maximum_scale) {
    if (!image.view || image.width <= 0 || image.height <= 0 || available.x < 1 || available.y < 21)
        return false;
    const float scale =
        std::min(maximum_scale, std::min(available.x / image.width, (available.y - 20) / image.height));
    const float margin = (available.x - image.width * scale) * .5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, margin));
    auto origin = ImGui::GetCursorScreenPos();
    ImGui::Image(image.view, ImVec2(image.width * scale, image.height * scale));
    auto* draw = ImGui::GetWindowDrawList();
    const auto& visual = settings.visual;
    const auto color = [&](const std::array<float, 3>& rgb) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], visual.opacity));
    };
    draw->PushClipRect(origin, ImVec2(origin.x + image.width * scale, origin.y + image.height * scale), true);
    for (auto& d : picture.detections) {
        if (visual.boxes)
            draw->AddRect(ImVec2(origin.x + d.x * scale, origin.y + d.y * scale),
                          ImVec2(origin.x + (d.x + d.w) * scale, origin.y + (d.y + d.h) * scale),
                          color(visual.box_color), visual.corner_radius, 0, visual.thickness);
        char label[64];
        std::string object = simulated                                    ? "Example"
                             : d.cls >= 0 && size_t(d.cls) < names.size() ? names[d.cls]
                                                                          : "Object " + std::to_string(d.cls);
        std::snprintf(label, sizeof(label), "%s  %.0f%%", object.c_str(), d.score * 100);
        if (visual.confidence)
            draw->AddText(ImGui::GetFont(), visual.font_size,
                          ImVec2(origin.x + d.x * scale, origin.y + d.y * scale), color(visual.box_color),
                          label);
        if (visual.tracers) {
            const float from = visual.tracer_origin == 0 ? 1.f : visual.tracer_origin == 1 ? .5f : 0.f;
            draw->AddLine(
                ImVec2(origin.x + image.width * scale * .5f, origin.y + image.height * scale * from),
                ImVec2(origin.x + (d.x + d.w * .5f) * scale, origin.y + (d.y + d.h * .5f) * scale),
                color(visual.box_color), visual.thickness);
        }
    }
    float rx = settings.reference_x < 0 ? image.width * .5f : settings.reference_x,
          ry = settings.reference_y < 0 ? image.height * .5f : settings.reference_y;
    if (visual.fov)
        draw->AddCircle(
            ImVec2(origin.x + rx * scale, origin.y + ry * scale),
            (settings.tracking.dynamic_fov && (stats.physical & (1u << (settings.tracking.fov_button - 1)))
                 ? settings.tracking.held_radius
                 : settings.fov_radius) *
                scale,
            color(visual.fov_color), 0, visual.thickness);
    if (picture.correction.target)
        draw->AddCircleFilled(
            ImVec2(origin.x + picture.correction.aim_x * scale, origin.y + picture.correction.aim_y * scale),
            4, IM_COL32(255, 100, 80, 255));
    if (settings.color.mode && settings.color.preview && picture.colors) {
        // Reuse target selection so preview agrees with class preferences and fallback.
        auto view_settings = settings;
        view_settings.tracking.prediction = false;
        receiver::Controller view_controller(0);
        receiver::Telemetry limits;
        limits.physical = stats.physical;
        limits.xmin = limits.ymin = -127;
        limits.xmax = limits.ymax = 127;
        const auto aim = view_controller.update(picture.detections, image.width, image.height, view_settings,
                                                limits, receiver::now_ns(), {}, 0, picture.colors.get());
        if (aim.target) {
            ImVec2 at(origin.x + aim.aim_x * scale, origin.y + aim.aim_y * scale);
            draw->AddLine(ImVec2(at.x - 6, at.y), ImVec2(at.x + 6, at.y), IM_COL32(255, 220, 80, 255), 2);
            draw->AddLine(ImVec2(at.x, at.y - 6), ImVec2(at.x, at.y + 6), IM_COL32(255, 220, 80, 255), 2);
        }
    }
    draw->PopClipRect();
    if (settings.color.mode && picture.colors)
        ImGui::Text("HSV matches: %llu | yellow cross: selected aim / fallback",
                    static_cast<unsigned long long>(picture.colors->count));
    return true;
}
} // namespace receiver::gui
