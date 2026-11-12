#include "preview_window.hpp"
#include "gui_sdl_support.hpp"
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

namespace receiver::gui {
void PreviewWindow::open(SDL_Window* main_window, SDL_GLContext main_context, bool hidden) {
    if (window_)
        return;
    auto* main_gui = ImGui::GetCurrentContext();
    SDL_GL_MakeCurrent(main_window, main_context);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    window_ = SDL_CreateWindow("Receiver Preview", x_, y_, saved_width_, saved_height_,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                   (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
    if (window_)
        context_ = SDL_GL_CreateContext(window_);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!window_ || !context_) {
        const std::string message = SDL_GetError();
        close();
        SDL_GL_MakeCurrent(main_window, main_context);
        throw std::runtime_error(message);
    }
    SDL_SetWindowMinimumSize(window_, 320, 240);
    SDL_GL_SetSwapInterval(0);
    SDL_GL_MakeCurrent(main_window, main_context);
    gui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(main_gui);
    try {
        ContextScope scope(window_, context_, gui_);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        ImGui::StyleColorsDark();
        if (!ImGui_ImplSDL2_InitForOpenGL(window_, context_))
            throw std::runtime_error("Cannot initialize preview input");
        platform_ready_ = true;
        if (!ImGui_ImplOpenGL3_Init("#version 130"))
            throw std::runtime_error("Cannot initialize preview renderer");
        renderer_ready_ = true;
        close_requested_ = false;
        ++openings_;
    } catch (...) {
        close();
        throw;
    }
}
void PreviewWindow::close() {
    if (window_) {
        SDL_GetWindowPosition(window_, &x_, &y_);
        SDL_GetWindowSize(window_, &saved_width_, &saved_height_);
    }
    if (gui_) {
        ContextScope scope(window_, context_, gui_);
        if (renderer_ready_)
            ImGui_ImplOpenGL3_Shutdown();
        if (platform_ready_)
            ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(gui_);
        gui_ = nullptr;
    }
    renderer_ready_ = platform_ready_ = drawn_ = close_requested_ = false;
    if (context_) {
        SDL_GL_DeleteContext(context_);
        context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    width_ = height_ = 0;
}
bool PreviewWindow::update_options(const InterfaceSettings& settings) {
    SDL_SetWindowAlwaysOnTop(window_, settings.always_on_top ? SDL_TRUE : SDL_FALSE);
    return true;
}
void PreviewWindow::process_event(const SDL_Event& event) {
    if (!window_)
        return;
    if (event.type == SDL_WINDOWEVENT && event.window.windowID == SDL_GetWindowID(window_)) {
        if (event.window.event == SDL_WINDOWEVENT_CLOSE)
            close_requested_ = true;
        auto* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(gui_);
        ImGui_ImplSDL2_ProcessEvent(&event);
        ImGui::SetCurrentContext(previous);
    }
}
void PreviewWindow::render(const PreviewImage& image, const Preview& picture, const Settings& settings,
                           const Stats& stats, const std::vector<std::string>& names, bool simulated) {
    drawn_ = false;
    if (!window_ || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED))
        return;
    ContextScope scope(window_, context_, gui_);
    SDL_GL_GetDrawableSize(window_, &width_, &height_);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Preview image", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted(stats.status.c_str());
    if (picture.frame && draw_preview_image(image, picture, settings, stats, names, simulated,
                                            ImGui::GetContentRegionAvail(), 128))
        ++rendered_frames_;
    else
        ImGui::TextWrapped("Waiting for video. Start a session to see the preview.");
    ImGui::End();
    ImGui::Render();
    glViewport(0, 0, width_, height_);
    glClearColor(.035f, .05f, .075f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    drawn_ = true;
}
void PreviewWindow::present() {
    if (!drawn_)
        return;
    ContextScope scope(window_, context_, gui_);
    SDL_GL_SwapWindow(window_);
    drawn_ = false;
}
void PreviewWindow::save_frame(const char* path) {
    ContextScope scope(window_, context_, gui_);
    save_gl_frame(path, width_, height_);
}
} // namespace receiver::gui
