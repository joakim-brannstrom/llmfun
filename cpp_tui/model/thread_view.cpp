/// Thread-state derivation. See thread_view.h for the precedence table.
#include "thread_view.h"

namespace llmfun::tui::model {

ThreadView deriveThreadView(bool hasMessages, bool loadingSession, bool offline) {
    if (loadingSession)
        return ThreadView::Loading;
    if (hasMessages)
        return ThreadView::Ready;
    if (offline)
        return ThreadView::Offline;
    return ThreadView::Empty;
}

} // namespace llmfun::tui::model
