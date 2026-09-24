#include "maya/platform/desktop_application.hpp"
#include "maya/core/engine.hpp"
#include "maya/core/file_system.hpp"
#include "maya/platform/input.hpp"
#include "maya/platform/window.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace maya {

int run_desktop(int argc, char** argv, std::unique_ptr<Application> application,
    const DesktopOptions& options) {
    std::optional<uint32_t> smoke_frames;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help") {
            std::cout << "Usage: " << argv[0] << " [--smoke [positive frame count]]\n";
            return 0;
        }
        if (argument != "--smoke" || smoke_frames) {
            std::cerr << "[Application] unknown or duplicate argument: " << argument << '\n';
            return 2;
        }
        smoke_frames = 120;
        if (i + 1 < argc) {
            const std::string_view count(argv[++i]);
            uint32_t frames = 0;
            const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), frames);
            if (error != std::errc{} || end != count.data() + count.size() || frames == 0) {
                std::cerr << "[Application] --smoke requires a positive frame count\n";
                return 2;
            }
            smoke_frames = frames;
        }
    }

    FileSystem::initialize(argc, argv);
    Input::instance().reset();
    // Reverse destruction keeps the window alive through engine shutdown.
    Window window(options.width, options.height, options.title);
    if (!window.get_native_handle()) {
        std::cerr << "[Application] window creation failed\n";
        return 1;
    }
    Engine engine;
    Input::instance().set_services({[&window] { return window.clipboard_text(); },
                                    [&window](const std::string& text) { window.set_clipboard_text(text); },
                                    [&window](CursorShape shape) { window.set_cursor_shape(shape); }});
    const auto publish_metrics = [&window] {
        const auto [points_width, points_height] = window.window_size();
        const auto [pixels_width, pixels_height] = window.framebuffer_size();
        Input::instance().set_window_metrics({float(points_width), float(points_height),
            uint32_t(std::max(pixels_width, 0)), uint32_t(std::max(pixels_height, 0))});
    };
    publish_metrics();
    if (!engine.initialize(GraphicsDevice::create_default(), window.get_native_handle(),
            std::move(application), options.device)) return 1;

    bool resize_ok = true;
    window.set_framebuffer_resize_callback([&](int width, int height) {
        if (width > 0 && height > 0)
            resize_ok = engine.resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    });
    const auto [width, height] = window.framebuffer_size();
    if (!engine.resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height))) return 1;
    auto cursor_captured = options.capture_cursor && !smoke_frames;
    window.set_cursor_captured(cursor_captured);

    auto last_time = std::chrono::steady_clock::now();
    auto last_title = last_time;
    float fps_smooth = 0.0f;
    uint32_t frame_count = 0;
    int result = 0;
    while (!window.should_close()) {
        window.poll_events();
        if (!resize_ok) { result = 1; break; }
        if (window.should_close() ||
            (!smoke_frames && options.escape_closes && Input::instance().is_key_pressed(KeyCode::Escape)))
            break;
        publish_metrics();
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (!engine.tick(smoke_frames ? 1.0f / 60.0f : elapsed, !smoke_frames)) {
            result = 1;
            break;
        }
        if (const auto capture = Input::instance().take_cursor_capture_request();
            capture && *capture != cursor_captured && !smoke_frames) {
            cursor_captured = *capture;
            window.set_cursor_captured(cursor_captured);
        }
        Input::instance().update();
        ++frame_count;
        if (elapsed > 0.0f) {
            const auto fps = 1.0f / elapsed;
            fps_smooth = fps_smooth == 0.0f ? fps : fps_smooth * 0.92f + fps * 0.08f;
        }
        // Refresh the title a few times a second; per-frame updates make it flicker.
        if (now - last_title < std::chrono::milliseconds(250) && frame_count > 1) {
            if (smoke_frames && frame_count >= *smoke_frames) break;
            continue;
        }
        last_title = now;
        const auto [fb_width, fb_height] = window.framebuffer_size();
        std::ostringstream title;
        title << options.title << " | " << std::fixed << std::setprecision(1)
              << fps_smooth << " fps | " << elapsed * 1000.0f << " ms | "
              << fb_width << 'x' << fb_height;
        window.set_title(title.str());
        if (smoke_frames && frame_count >= *smoke_frames) break;
    }
    window.set_framebuffer_resize_callback({});
    engine.shutdown();
    Input::instance().reset();
    if (smoke_frames) {
        std::cerr << "[Application] smoke: completed " << frame_count << '/' << *smoke_frames << " frames\n";
        if (frame_count != *smoke_frames) result = 1;
    }
    return result;
}

} // namespace maya
