/// The thread region: a bounded window of the thread's groups, mounted in the
/// shell (design §4.5, D-BOUNDS, D-FOLLOW).
///
/// The groups come from `store::ThreadStoreAdapter` - the only v4-aware code
/// the shell touches (D-ADAPTER) - and this file never sees a message: it asks
/// how many groups there are, which one sits at an index, how tall it expects to
/// be and then asks the adapter to draw it. That is the whole seam, which is why
/// the message renderer could be *mounted* here without being rewritten.
///
/// Bounded, not lazy-paged (D-BOUNDS): the window is the last `windowGroups` *stored* groups plus
/// the pages the `Load earlier` row has revealed, and the live answer is drawn after it while a
/// stream runs (task 38) - a live answer can never be paged away, and `UiState::thread.drawnGroups`
/// counts it too. The row is drawn in the rect the layout engine answered for it
/// (`ShellLayout::loadEarlier`, the thread's first row) and is hidden while everything fits, so a
/// short thread never pays for a control it cannot use.
///
/// Follow the newest (D-FOLLOW): the flag is `model::UiState::follow` and the
/// policy is `ui::followNewest()` - the one implementation of it, so "at the
/// bottom" cannot mean two things.
#pragma once

#include <cstddef>

namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui::model {
struct UiState;
enum class ThreadView;
} // namespace llmfun::tui::model

namespace llmfun::tui::store {
class ThreadStoreAdapter;
} // namespace llmfun::tui::store

namespace llmfun::tui::views {

/// The groups the thread draws this frame.
///
/// `first` is the index of the first drawn group, `count` how many are drawn and
/// `hidden` how many remain above `first` - the number the `Load earlier` row
/// offers. `count == 0` means the thread has nothing to draw (the empty body
/// takes over there).
struct ThreadWindow {
    std::size_t first = 0;
    std::size_t count = 0;
    std::size_t hidden = 0;
};

/// The window of a thread with `groupCount` groups: the last
/// `windowGroups + earlierGroups`, clamped into `[0, groupCount]`.
///
/// Pure, so the paging arithmetic can be tested without a renderer. A
/// non-positive `windowGroups` is treated as one group (a window that shows
/// nothing would look like a bug, not a policy) and a negative `earlierGroups`
/// as none.
ThreadWindow threadWindow(std::size_t groupCount, int windowGroups, int earlierGroups);

/// How many groups one `Load earlier` press reveals: one whole window, so a
/// press is a page rather than a row.
int loadEarlierPage(int windowGroups);

/// Draw the thread region.
///
/// `region` is `ShellLayout::threadColumn` (the whole thread region, or its centred column in focus
/// mode - task 43) and `loadEarlierRow` its first row (both in the metrics' units; a rectangle in
/// the wrong unit paints nothing - the adapter's own guard is the second half of that rule).
/// `windowGroups` is
/// `ShellLayout::compactGroups`, i.e. the layout's answer for this frame, never a
/// constant this view picks, and `compact` is `ShellLayout::compact` - the flag the
/// height-hint cache is keyed by, because compact mode draws less of a group.
///
/// A group is drawn as a list of *blocks* (design §4.5, task 26): the group's own
/// header row, then a message's thinking disclosure and its body row - or, for a work
/// group, one row per step. `views::renderBlock()` draws each row's chrome (role rule,
/// label, duration, state glyph, focus, copy control) and the adapter draws the body
/// under it, so the one file that knows markdown keeps drawing bodies and this one
/// never does. A body that is not the ordinary thread (`model::ThreadView::Empty`,
/// `Loading`, `Offline`) is a short static card here, drawn from the model's own copy.
///
/// `acceptKeys` is the shell's answer to "may a view-local key act this frame" - false
/// while an overlay owns the keyboard, or when a press was already spent (the rail takes
/// its own row keys under the same flag). It gates exactly one key here: `y`, which
/// copies the focused block (task 26).
void renderThread(model::UiState& ui, store::ThreadStoreAdapter& thread, const ui::Rect& region,
                  const ui::Rect& loadEarlierRow, const ui::Metrics& m, int windowGroups,
                  bool compact, bool acceptKeys);

/// Reveal one page of older groups: what `PgUp` / `Ctrl+U` and the palette's
/// `Load earlier` row do (D-BOUNDS, D-PALETTE).
///
/// The page is the window the thread was last drawn with
/// (`UiState::thread.pageGroups`), so one press is one page in every mode - a fixed
/// 40 would reveal the whole thread in compact mode, where the window is one or
/// three groups. Pure: no renderer and no layout, so the paging arithmetic is
/// testable without a frame.
void requestEarlierPage(model::UiState& ui);

} // namespace llmfun::tui::views
