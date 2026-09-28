#pragma once

#include <memory>

namespace acecode::desktop {

// Call at process entry, before GTK or any other code opens an X11 display.
// Qt initializes Xlib threading too; doing that after GTK has created its
// resource databases leaves their locks uninitialized on older Xlib versions.
void initialize_deepin_windowing();

// Optional DTK frame effects. GTK continues to own the window, input and
// controls; no Qt widgets or titlebar are placed over the WebView.
class DeepinWindowEffects {
public:
    explicit DeepinWindowEffects(void* gtk_window);
    ~DeepinWindowEffects();
    DeepinWindowEffects(const DeepinWindowEffects&) = delete;
    DeepinWindowEffects& operator=(const DeepinWindowEffects&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::desktop
