/// The session action queue (session_panel.h).
///
/// What used to live beside this file was the legacy sidebar's renderer; the rail draws
/// the list now (views/rail.cpp) and owns the click, so only the queue protocol is left
/// here.

#include "session_panel.h"

namespace llmfun::tui {

void queueSessionAction(ChatTabSessionPanel& panel, SessionActionType type, const std::string& id,
                        const std::string& title) {
    panel.actions.push_back(SessionAction{type, id, title});
}

} // namespace llmfun::tui
