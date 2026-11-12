#pragma once
#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace receiver::gui {
// Preview rendering borrows shared textures while keeping each window's renderer state isolated.
struct ContextScope {
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    SDL_GLContext context = SDL_GL_GetCurrentContext();
    ImGuiContext* gui = ImGui::GetCurrentContext();
    ContextScope(SDL_Window* next_window, SDL_GLContext next_context, ImGuiContext* next_gui) {
        if (SDL_GL_MakeCurrent(next_window, next_context) != 0)
            throw std::runtime_error(SDL_GetError());
        ImGui::SetCurrentContext(next_gui);
    }
    ~ContextScope() {
        ImGui::SetCurrentContext(gui);
        SDL_GL_MakeCurrent(window, context);
    }
};
inline void save_gl_frame(const char* path, int width, int height) {
    if (width <= 0 || height <= 0)
        throw std::runtime_error("Cannot save an empty GUI framebuffer");
    std::vector<uint8_t> pixels(size_t(width) * height * 4), flipped(pixels.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const size_t stride = size_t(width) * 4;
    for (int y = 0; y < height; ++y)
        std::copy_n(pixels.data() + size_t(height - y - 1) * stride, stride,
                    flipped.data() + size_t(y) * stride);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(flipped.data(), width, height, 32, width * 4,
                                                              SDL_PIXELFORMAT_RGBA32);
    if (!surface)
        throw std::runtime_error(SDL_GetError());
    const int result = SDL_SaveBMP(surface, path);
    SDL_FreeSurface(surface);
    if (result != 0)
        throw std::runtime_error(SDL_GetError());
}
} // namespace receiver::gui
