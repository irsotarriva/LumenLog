#ifndef LUMEN_SRC_DASHBOARD_FRAME_HPP
#define LUMEN_SRC_DASHBOARD_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "lumen/dashboard_model.hpp"
#include "lumen/terminal_style.hpp"

// Drawing one dashboard frame with FTXUI. The only FTXUI-dependent code in
// Lumen; compiled with LUMEN_ENABLE_DASHBOARD only.

namespace lumen::detail {

struct FrameSettings {
    uint16_t width = 0;   // 0: 80 columns (the sink measures the terminal and fills these in)
    uint16_t height = 0;  // 0: 24 rows
    std::size_t panel_rows = 8;
    LevelColors colors = default_level_colors;
};

// The frame as text to write at the top-left corner of the screen: one row
// per screen line, each ending by clearing the rest of the line, with FTXUI's
// colour escapes. May throw whatever FTXUI throws.
[[nodiscard]] std::string render_frame(const DashboardModel& model, const FrameSettings& settings);

}  // namespace lumen::detail

#endif
