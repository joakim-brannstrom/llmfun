/// Session panel actions: the one place that knows the sidebar's queue protocol.
///
/// The rail's own rows, the palette's SESSIONS rows and its New-session action all go
/// through `queueSessionAction`, so a session switch cannot reach D two ways.
#pragma once

#include <string>

#include "tui.h"
#include "tui_common.h"

namespace llmfun::tui {

/// Queue one sidebar action for the D side to poll.
void queueSessionAction(ChatTabSessionPanel& panel, SessionActionType type, const std::string& id,
                        const std::string& title);

} // namespace llmfun::tui
