/// The session rail's list model (session_rows.h owns the contract).
#include "session_rows.h"

#include "session_fuzzy.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace llmfun::tui::model {
namespace {

constexpr long long kMsPerDay = 86'400'000LL;

/// Empty or whitespace-only = "no filter". The shipped panel's rule, kept: a
/// query of spaces is not a query.
bool filterIsEmpty(std::string_view filter) {
    for (const char c : filter) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

/// Where a row belongs by date, from the two civil-day indices.
Group dateGroup(int rowDay, int todayDay) {
    const int delta = todayDay - rowDay;
    if (delta <= 0)
        return Group::Today; // today, or a future stamp (clock skew): not "older"
    if (delta == 1)
        return Group::Yesterday;
    if (delta <= 7)
        return Group::Previous7;
    return Group::Older;
}

/// Pinned first, then newest first, then title - one comparator for every group,
/// so two rows that tie on time cannot depend on the snapshot's order.
bool rowOrder(const SessionRow& a, const SessionRow& b) {
    if (a.pinned != b.pinned)
        return a.pinned; // only reachable in a flat list, where pinned is not its own group
    if (a.updatedAtMs != b.updatedAtMs)
        return a.updatedAtMs > b.updatedAtMs;
    if (a.title != b.title)
        return a.title < b.title;
    return a.id < b.id;
}

} // namespace

const char* groupLabel(Group group) {
    switch (group) {
    case Group::Pinned:
        return "PINNED";
    case Group::Today:
        return "TODAY";
    case Group::Yesterday:
        return "YESTERDAY";
    case Group::Previous7:
        return "PREVIOUS 7 DAYS";
    case Group::Older:
        return "OLDER";
    case Group::Ungrouped:
        return "SESSIONS";
    case Group::Count:
        break;
    }
    return "SESSIONS";
}

int civilDay(long long unixMs) {
    // A *day ordinal*, i.e. days since 1970-01-01 in the local calendar, so that two
    // consecutive civil days always differ by exactly 1 - across a month end, a year
    // end and a DST edge. The local date is read as if it were UTC and divided by a
    // day: the broken-down date is a calendar day (no time of day, no DST), so the
    // division is exact and `mktime`'s DST-dependent hour shifts never enter.
    //
    // Encoding the date as `year * 10000 + month * 100 + day` instead - what this did
    // first - is *not* a day number: 2026-11-01 minus 2026-10-31 is 70, so every row
    // from the previous month was filed under OLDER for the first week of a month.
    const std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
    const std::time_t dayStart = _mkgmtime(&local);
#else
    localtime_r(&seconds, &local);
    const std::time_t dayStart = timegm(&local);
#endif
    return static_cast<int>(dayStart / 86'400);
}

RailModel buildRailRows(const std::vector<SessionRow>& rows, long long nowMs,
                        std::string_view filter) {
    RailModel model;

    // --- filter ------------------------------------------------------------
    std::vector<SessionRow> matched;
    const bool filtering = !filterIsEmpty(filter);
    if (!filtering) {
        matched = rows; // a copy: the groups own their rows and the caller may reuse `rows`
    } else {
        const std::string query(filter);
        struct Scored {
            const SessionRow* row;
            int score;
        };
        std::vector<Scored> scored;
        scored.reserve(rows.size());
        for (const SessionRow& row : rows) {
            const int score = fuzzyScoreFields(query, row.title, row.preview);
            if (score >= 0)
                scored.push_back(Scored{&row, score});
        }
        // Best score first; `stable_sort` keeps snapshot order for equal scores,
        // which is the shipped panel's tie rule ("stable ties keep snapshot order").
        std::stable_sort(scored.begin(), scored.end(),
                         [](const Scored& a, const Scored& b) { return a.score > b.score; });
        matched.reserve(scored.size());
        for (const Scored& entry : scored)
            matched.push_back(*entry.row);
    }
    model.filteredOut = rows.size() - matched.size();

    // --- flat or grouped? --------------------------------------------------
    // A filter answers a ranked list (no groups), and so does a snapshot with any
    // unknown timestamp: there is no honest date to put such a row under, and
    // inventing one would sort the rail by a fact nobody has (D-SESSION-TS).
    model.flat = filtering;
    if (!model.flat) {
        model.flat = std::any_of(matched.begin(), matched.end(),
                                 [](const SessionRow& row) { return row.updatedAtMs <= 0; });
    }

    if (model.flat) {
        std::sort(matched.begin(), matched.end(), rowOrder);
        RailGroup group;
        group.group = Group::Ungrouped;
        group.rows = std::move(matched);
        model.rowCount = group.rows.size();
        if (!group.rows.empty())
            model.groups.push_back(std::move(group));
        return model;
    }

    // --- group by date -----------------------------------------------------
    const int today = civilDay(nowMs);
    RailGroup buckets[static_cast<std::size_t>(Group::Count)];
    for (std::size_t i = 0; i < static_cast<std::size_t>(Group::Count); ++i)
        buckets[i].group = static_cast<Group>(i);

    for (SessionRow& row : matched) {
        const Group target =
            row.pinned ? Group::Pinned : dateGroup(civilDay(row.updatedAtMs), today);
        buckets[static_cast<std::size_t>(target)].rows.push_back(std::move(row));
    }

    // Display order is the enum's order (Pinned first, then newest date group).
    // A pinned row is *only* in PINNED: the design's group is a group, not a mark,
    // and a row that appeared twice would make the scroll extent lie.
    for (std::size_t i = 0; i < static_cast<std::size_t>(Group::Count); ++i) {
        RailGroup& bucket = buckets[i];
        if (bucket.group == Group::Ungrouped || bucket.rows.empty())
            continue;
        std::sort(bucket.rows.begin(), bucket.rows.end(), rowOrder);
        model.rowCount += bucket.rows.size();
        model.groups.push_back(std::move(bucket));
    }
    return model;
}

void UnreadTracker::apply(const std::vector<SessionRow>& rows) {
    std::set<std::string> nextUnread;
    std::map<std::string, long long> nextSeen;
    std::string nextActive;

    for (const SessionRow& row : rows) {
        if (row.id.empty())
            continue;
        if (row.active)
            nextActive = row.id;
        const auto previous = seen_.find(row.id);
        const bool known = previous != seen_.end() && previous->second > 0;
        // The row the user just left arrives with the stamp its own farewell wrote:
        // `switchToSession` commits the outgoing session and the store bumps
        // `updatedAt` on every save. That advance is the baseline, not news.
        const bool justLeft = !row.active && !lastActiveId_.empty() && row.id == lastActiveId_;
        const bool advanced = known && row.updatedAtMs > previous->second && !justLeft;
        // Selecting a session clears its mark; a session that is not on screen can
        // never be "unseen", and an unknown time is not evidence of an update.
        if (!row.active && (advanced || unread_.count(row.id) != 0))
            nextUnread.insert(row.id);
        if (row.updatedAtMs > 0)
            nextSeen[row.id] = row.updatedAtMs;
        else if (known)
            nextSeen[row.id] = previous->second; // an unknown stamp never erases what we knew
    }

    if (nextUnread != unread_)
        unread_ = std::move(nextUnread);
    seen_ = std::move(nextSeen);
    lastActiveId_ = std::move(nextActive);
}

void UnreadTracker::clear(const std::string& id) { unread_.erase(id); }

void UnreadTracker::reset() {
    unread_.clear();
    seen_.clear();
    lastActiveId_.clear();
}

} // namespace llmfun::tui::model
