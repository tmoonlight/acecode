#pragma once
#include "tui/input/ports.hpp"

namespace acecode::tui {
class TuiClipboard final : public IClipboard {
public:
    ClipboardTextReadResult read_text() override;
    ClipboardImageReadResult read_image() override;
    ClipboardTextWriteResult write_text(const std::string& text) override;
    void write_osc52(const std::string& text) override;
};
}
