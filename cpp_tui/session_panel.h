/// Session sidebar panel.
/// Renders the session list (rows, filter, rename, delete) from TuiState.
#pragma once

#include "tui.h"
#include "tui_common.h"

namespace llmfun::tui {

void renderTabChatSessionPanel(TuiState& state, Log& log);

} // namespace llmfun::tui
