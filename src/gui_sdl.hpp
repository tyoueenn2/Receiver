#pragma once
#include "preview_window.hpp"
#include "gui_sdl_support.hpp"
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

namespace {
using NativeWindow = SDL_Window*;
struct Texture {
    GLuint view = 0;
    int w = 0, h = 0;
    uint64_t session = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> rgba;
    ImTextureID id() const {
        return ImTextureID(view);
    }
    ~Texture() {
        clear();
    }
    void clear() {
        if (view)
            glDeleteTextures(1, &view);
        view = 0;
        w = h = 0;
        session = 0;
        rgba.clear();
    }
    void update(const receiver::Frame& frame) {
        if (view && session == frame.header.session && sequence == frame.header.sequence)
            return;
        if (w != frame.header.width || h != frame.header.height) {
            clear();
            w = frame.header.width;
            h = frame.header.height;
            glGenTextures(1, &view);
            glBindTexture(GL_TEXTURE_2D, view);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            rgba.resize(size_t(w) * h * 4);
        }
        const int channels = frame.header.format == 1 ? 3 : 4;
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            rgba[i * 4] = frame.pixels[i * channels + (channels == 4 ? 2 : 0)];
            rgba[i * 4 + 1] = frame.pixels[i * channels + 1];
            rgba[i * 4 + 2] = frame.pixels[i * channels + (channels == 4 ? 0 : 2)];
            rgba[i * 4 + 3] = 255;
        }
        GLint previous = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        glBindTexture(GL_TEXTURE_2D, view);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, GLuint(previous));
        session = frame.header.session;
        sequence = frame.header.sequence;
    }
};
// Preserve profile key codes across platforms; only focused-window shortcuts are read on Linux.
SDL_Keycode sdl_key(int key) {
    if (key >= 'A' && key <= 'Z')
        return SDLK_a + key - 'A';
    if (key >= '0' && key <= '9')
        return SDLK_0 + key - '0';
    if (key >= 0x70 && key <= 0x7B)
        return SDLK_F1 + key - 0x70;
    switch (key) {
    case 8:
        return SDLK_BACKSPACE;
    case 9:
        return SDLK_TAB;
    case 13:
        return SDLK_RETURN;
    case 16:
        return SDLK_LSHIFT;
    case 17:
        return SDLK_LCTRL;
    case 18:
        return SDLK_LALT;
    case 27:
        return SDLK_ESCAPE;
    case 32:
        return SDLK_SPACE;
    case 33:
        return SDLK_PAGEUP;
    case 34:
        return SDLK_PAGEDOWN;
    case 35:
        return SDLK_END;
    case 36:
        return SDLK_HOME;
    case 37:
        return SDLK_LEFT;
    case 38:
        return SDLK_UP;
    case 39:
        return SDLK_RIGHT;
    case 40:
        return SDLK_DOWN;
    case 45:
        return SDLK_INSERT;
    case 46:
        return SDLK_DELETE;
    case 186:
        return SDLK_SEMICOLON;
    case 187:
        return SDLK_EQUALS;
    case 188:
        return SDLK_COMMA;
    case 189:
        return SDLK_MINUS;
    case 190:
        return SDLK_PERIOD;
    case 191:
        return SDLK_SLASH;
    case 192:
        return SDLK_BACKQUOTE;
    case 219:
        return SDLK_LEFTBRACKET;
    case 220:
        return SDLK_BACKSLASH;
    case 221:
        return SDLK_RIGHTBRACKET;
    case 222:
        return SDLK_QUOTE;
    default:
        return SDLK_UNKNOWN;
    }
}
struct GuiBackend {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    bool platform_ready = false, renderer_ready = false;
    ~GuiBackend() {
        close();
    }
    void open(bool compact, bool smoke) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
            throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window = SDL_CreateWindow("UDP Vision Receiver", 80, 60, compact ? 1060 : 1280, compact ? 740 : 900,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                      (smoke ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        if (!window)
            throw std::runtime_error(SDL_GetError());
        SDL_SetWindowMinimumSize(window, 1060, 740);
        context = SDL_GL_CreateContext(window);
        if (!context)
            throw std::runtime_error(SDL_GetError());
        SDL_GL_SetSwapInterval(0);
    }
    void initialize_imgui() {
        if (!ImGui_ImplSDL2_InitForOpenGL(window, context))
            throw std::runtime_error("Cannot initialize SDL input");
        platform_ready = true;
        if (!ImGui_ImplOpenGL3_Init("#version 130"))
            throw std::runtime_error("Cannot initialize the OpenGL renderer");
        renderer_ready = true;
    }
    bool poll(receiver::gui::PreviewWindow& preview) {
        SDL_Event event;
        bool running = true;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            preview.process_event(event);
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                 event.window.windowID == SDL_GetWindowID(window)))
                running = false;
        }
        return running;
    }
    void set_topmost(bool enabled) {
        SDL_SetWindowAlwaysOnTop(window, enabled ? SDL_TRUE : SDL_FALSE);
    }
    bool set_capture_exclusion(bool) {
        return true;
    } // The control is unavailable on Linux.
    void new_frame() {
        SDL_GL_MakeCurrent(window, context);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
    }
    bool focused() const {
        return SDL_GetKeyboardFocus() == window;
    }
    bool key_down(int key) const {
        const auto code = sdl_key(key);
        return code != SDLK_UNKNOWN && SDL_GetKeyboardState(nullptr)[SDL_GetScancodeFromKey(code)];
    }
    std::string key_name(int key) const {
        const auto code = sdl_key(key);
        return code == SDLK_UNKNOWN ? "Key " + std::to_string(key) : SDL_GetKeyName(code);
    }
    void show_preview(receiver::gui::PreviewWindow& preview) {
        SDL_RestoreWindow(preview.handle());
        SDL_RaiseWindow(preview.handle());
    }
    void open_preview(receiver::gui::PreviewWindow& preview, bool hidden) {
        preview.open(window, context, hidden);
    }
    bool render() {
        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(.06f, .07f, .09f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        return true;
    }
    void save_frame(const char* path, receiver::gui::PreviewWindow* preview = nullptr) {
        if (preview) {
            preview->save_frame(path);
            return;
        }
        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(window, &width, &height);
        receiver::gui::save_gl_frame(path, width, height);
    }
    void present() {
        SDL_GL_SwapWindow(window);
    }
    void close() {
        if (context)
            SDL_GL_MakeCurrent(window, context);
        if (renderer_ready) {
            ImGui_ImplOpenGL3_Shutdown();
            renderer_ready = false;
        }
        if (platform_ready) {
            ImGui_ImplSDL2_Shutdown();
            platform_ready = false;
        }
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        if (context) {
            SDL_GL_DeleteContext(context);
            context = nullptr;
        }
        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
    }
};
} // namespace
