/// Chat and log render cluster.
/// Renders the main window, the chat/log tabs, markdown, and agent stream.
#pragma once

#include "tui.h"
#include "tui_common.h"

namespace llmfun::tui {

void renderMainWindow(TuiState& state, Log& log);

} // namespace llmfun::tui
