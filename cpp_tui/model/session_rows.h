/// The session rail's list model: grouping, ordering, filtering and the
/// UI-local unread set (design §4.3, FR-03, D-RAIL, D-UNREAD, D-SESSION-TS).
///
///     model::RailModel list = model::buildRailRows(rows, nowMs, filter);
///     views::renderRail(ui.rail, list, layout.rail, m, rowLines, nowMs, acceptKeys);
///
/// Pure and clock-injected: the same `(rows, nowMs, filter)` always answers the
/// same groups, which is what lets a test cross a day, a month, a year and a week
/// boundary and the unknown-timestamp case without waiting for midnight. The module
/// holds no renderer, no ImGui and no `TuiState`; it does not even know the v4
/// snapshot type - the frame layer (`views/shell.cpp`) maps `SessionEntry` onto
/// `SessionRow`, and that is the only place the two meet.
///
/// Layering: the standard library only, and only same-directory headers. It is
/// reachable from `tui.h` (`model::UiState` carries the rail's state), so it may
/// not include `ui/` or `views/` - the rule `views/topbar.h` states.
#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llmfun::tui::model {

/// One rail row, as the frame layer hands it to the model.
///
/// `active`, `running`, `unread` and `pinned` are *state*, not identity: they
/// change what a row looks like, never where it sorts to (except `pinned`, which
/// moves it into the PINNED group - the design's own exception).
struct SessionRow {
    std::string id;
    std::string title;
    std::string preview;
    /// The row's total message count: never drawn on the row (the design removed
    /// the `[N]` suffix), only in the row's tooltip, where "how much is in here" is
    /// a detail rather than a label.
    std::size_t messageCount = 0;
    /// `SessionMeta.updatedAt * 1000` (D-SESSION-TS). 0 means unknown, and a
    /// snapshot with any unknown row is drawn flat - dates are never fabricated.
    long long updatedAtMs = 0;
    bool active = false;
    bool running = false;
    bool unread = false;
    bool pinned = false;
};

/// The design's date groups (§4.3), plus the two states that are not dates:
/// `Ungrouped` (a flat list, because the snapshot has no usable timestamps or
/// the filter collapsed the ordering) and `Pinned`.
///
/// Declaration order is the display order of the groups.
enum class Group {
    Pinned = 0,
    Today,
    Yesterday,
    Previous7,
    Older,
    Ungrouped,
    Count,
};

/// The header a group draws: "PINNED", "TODAY", "YESTERDAY", "PREVIOUS 7 DAYS",
/// "OLDER", "SESSIONS". The ungrouped list keeps the design's own word for a
/// list without dates rather than claiming to be a date group.
const char* groupLabel(Group group);

/// One group and its rows, in display order.
struct RailGroup {
    Group group = Group::Ungrouped;
    std::vector<SessionRow> rows;
};

/// The rail's list for one frame.
struct RailModel {
    std::vector<RailGroup> groups;
    /// Rows across all groups (what the scroll extent is measured in).
    std::size_t rowCount = 0;
    /// The list has no dates: one `Ungrouped` group, no headers claiming a day.
    /// True for an all-zero snapshot and for a filtered list.
    bool flat = false;
    /// Rows the filter dropped. The view turns `rowCount == 0 && filteredOut > 0`
    /// into the "no matches" line; without it an empty filter result and an empty
    /// snapshot would look the same.
    std::size_t filteredOut = 0;
};

/// Days since the Unix epoch in the **local calendar**: "TODAY" has to mean the
/// user's today, not UTC's (a session sent at 23:30 local is not "tomorrow").
///
/// A real *day ordinal*: consecutive civil days differ by exactly 1 across a month
/// end, a year end and a DST edge, which is what `buildRailRows()` compares. (A
/// `YYYYMMDD` number is not - 20261101 minus 20261031 is 70.)
///
/// Exposed because the grouping tests need the same function the grouping uses - with
/// `TZ` pinned to UTC they can then use plain epoch arithmetic and still be testing the
/// real rule.
int civilDay(long long unixMs);

/// Group, order and filter `rows`.
///
/// * `filter` that is empty or whitespace-only is "no filter": the rows are
///   grouped by date (or drawn flat when any timestamp is unknown) and ordered
///   pinned-first, then `updatedAtMs` descending, then title.
/// * A real filter keeps the rows whose title *or* preview matches
///   (`fuzzyScoreFields`, the fzf implementation `session_fuzzy.h` already
///   ships), ranks them by descending score with snapshot order as the tie
///   break, and answers one flat `Ungrouped` group - the shipped semantics of
///   the panel it replaces: a search is a ranked list, not a calendar.
///
/// Groups with no rows are omitted, and a group whose header would be the only
/// thing on screen therefore cannot exist.
RailModel buildRailRows(const std::vector<SessionRow>& rows, long long nowMs,
                        std::string_view filter);

/// The UI-local unread set (D-UNREAD, FR-03, D-19).
///
/// The rule, in one place: a row is unread when its `updatedAt` *advances* while
/// it is not the active session, during this process lifetime. There is no store
/// concept behind it and it is never persisted: `ui.conf` has no unread key by
/// design, and this type has no serializer.
///
/// The tracker also remembers the last time it saw per id (`seen()`), because
/// "advanced" needs a previous value - which is exactly why the state has to
/// live somewhere across snapshots. It also remembers which row *was* active in
/// the previous snapshot, because leaving a session is itself a write on the D
/// side (see the exemption in `apply()`).
class UnreadTracker {
public:
    /// Apply one snapshot. Order within `rows` does not matter.
    ///
    /// * a row whose id is new is *not* unread (nothing advanced, it just
    ///   appeared);
    /// * a row whose `updatedAtMs` grew while it was not active becomes unread;
    /// * the row that was active in the *previous* snapshot is exempt from that
    ///   test for one snapshot: leaving a session commits it (`switchToSession`)
    ///   and the store stamps `updatedAt` on every write, so the row the user just
    ///   left arrives with the newer stamp the switch itself produced. That is the
    ///   baseline, not an update nobody has seen - the dot belongs on rows that
    ///   moved while the user was somewhere else;
    /// * the active row is never unread (selecting it is what clears the mark,
    ///   and a session cannot be unseen while it is on screen);
    /// * an id that left the snapshot is forgotten entirely;
    /// * an unknown timestamp (0) never advances anything - `0 -> 0` and
    ///   `0 -> t` are not evidence of an update, so they mark nothing.
    void apply(const std::vector<SessionRow>& rows);

    /// Clear one id (the select path: the user is now looking at that session).
    void clear(const std::string& id);

    bool isUnread(const std::string& id) const { return unread_.count(id) != 0; }
    const std::set<std::string>& ids() const { return unread_; }
    std::size_t size() const { return unread_.size(); }

    void reset();

private:
    std::map<std::string, long long> seen_;
    std::set<std::string> unread_;
    /// The id that was active when `apply()` ran last time. `""` before the first
    /// snapshot, so the first frame exempts nobody.
    std::string lastActiveId_;
};

} // namespace llmfun::tui::model
