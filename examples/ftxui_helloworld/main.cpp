#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

int main() {
    using namespace ftxui;

    auto screen = ScreenInteractive::TerminalOutput();

    auto renderer = Renderer([&] {
        auto top = window(text("TOP") | bold | center,
                          text("Hello World") | center)
                 | size(HEIGHT, EQUAL, 5);

        auto middle = window(text("MIDDLE") | bold | center,
                             vbox({
                                 filler(),
                                 text("FTXUI three-row layout test") | center,
                                 filler(),
                             }));

        auto bottom = window(text("BOTTOM") | bold | center,
                             text("Press q or Esc to exit") | center)
                    | size(HEIGHT, EQUAL, 5);

        return vbox({
                   top,
                   middle | flex,
                   bottom,
               })
             | border;
    });

    auto app = CatchEvent(renderer, [&](const Event& event) {
        if (event == Event::Escape || event == Event::Character('q')) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    screen.Loop(app);
    return 0;
}
