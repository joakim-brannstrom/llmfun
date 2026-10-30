/// Follow-the-newest for a scrolling surface (D-FOLLOW, plan appendix §5.11).
///
/// The thread is a scroller that "stays at the newest content until the user
/// moves away". This file is the whole of that policy, in one place: the thread
/// region (views/thread.cpp) calls it, and the defect task 13a fixed cannot come
/// back in a second copy that did its own thing.
///
/// Three rules, and the order matters:
///
///   1. a pointer scroll wins - following stops, unless the pointer landed
///      exactly at the bottom again (dragging back down re-arms it);
///   2. otherwise the pin runs only when there is something to follow (content
///      new since this window's last `Begin`) or a catch-up is owed (`End`, a
///      session switch, a cleared thread). A pin on every frame - what this
///      replaced - leaves a target armed for the next frame, where it overwrites
///      the wheel notch and makes the position test self-confirming;
///   3. `ScrollMax` is one frame behind (`Begin()` computes it from the content
///      the previous frame drew), so "is there content below the fold" compares
///      the live content height against it rather than trusting the position
///      after a pin.
///
/// Call it inside the scroller's own window (after `BeginChild`, before
/// `EndChild`), and store the answer: it is the caller's flag for the next
/// frame, because the pointer can take the scroll away at any time.
///
/// Layer: `ui/` only - imgui and nothing else, so the module is a pure function
/// of the live ImGui window and can be linked by any surface.
#pragma once

namespace llmfun::tui::ui {

/// True while the pointer is scrolling the current window: a wheel notch over
/// it, or a drag/click on its vertical scrollbar.
///
/// The follower cannot ask the scroll position instead: ImGui applies the wheel
/// in `NewFrame()` (`UpdateMouseWheel()`), i.e. before this window's `Begin()`
/// applies the bottom target `SetScrollHereY()` armed on the previous frame, so
/// by the time view code runs the wheel is already undone. Input is the only
/// signal the pin cannot overwrite.
bool pointerTookOverScroll();

/// The follow-the-newest answer for the window that is current this frame.
/// `follow` is the flag the caller stored last frame; the return value is what
/// it should store now (`false` once the pointer took the scroll away, `true`
/// while the bottom stays pinned).
bool followNewest(bool follow);

} // namespace llmfun::tui::ui
