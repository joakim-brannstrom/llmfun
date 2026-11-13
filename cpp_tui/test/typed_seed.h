/// Typed-store seeding for the tests: the
/// plumbing every seeding call site shares, plus the message shapes the D
/// emitters send. The C upserts copy every string during the call, so these
/// `String`s are non-owning and never freed.
///
/// One `Seeder` per test: `add()` appends a message to the newest turn (a
/// `User` message opens one), `runStart`/`updateStream`/`runStop` drive a live
/// run the way `emitTurnStartTyped`/the stream updater/`emitStopTyped` do, and
/// `tool` hangs a merged tool card off the live batch. Turns order by seed
/// order - the store's insertion order is the turn sequence.
#pragma once

#include "../tui_api.h"
#include "tui_state_handle.h" // the ::TuiState handle shape the C upserts take

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace typed_seed {

/// A non-owning `String`: the C API copies during the call.
inline String makeStr(const char* text) {
    String s;
    s.data = text;
    s.len = text != nullptr ? std::strlen(text) : 0;
    return s;
}
inline String makeStr(const std::string& text) { return makeStr(text.c_str()); }

/// One seeding session: its own turn ids, its own live run.
struct Seeder {
    llmfun::tui::TuiState* state = nullptr;
    ::TuiState handle; ///< the C surface's one-pointer handle around `state`

    explicit Seeder(llmfun::tui::TuiState* s, std::int64_t firstTurnId = 1)
        : state(s), handle{s}, turnSeq(firstTurnId - 1) {}
    std::int64_t turnSeq = 0;
    std::int64_t liveTurnId = 0; ///< the in-flight run's turn (0 = none)
    int liveBatch = 0;           ///< the live run's batch counter
    int callSeq = 0;             ///< the live run's call counter (unique call ids)
    int noticeSeq = 0;           ///< the notice counter (unique notice ids)

    void upsertTurn(std::int64_t id, int state, int messageCount) {
        TuiTurn t{};
        t.id = id;
        t.state = state;
        t.messageCount = messageCount;
        tuiTurnUpsert(&handle, &t);
    }

    void upsertBlock(std::int64_t turnId, const std::string& id, const std::string& batchId,
                     int kind, int status, const char* meta, const std::string& body,
                     const char* title = nullptr, std::int64_t durationMs = 0,
                     std::int64_t tokensIn = 0, std::int64_t tokensOut = 0,
                     const std::vector<std::pair<std::string, std::string>>& resultPairs = {},
                     const std::string& resultRaw = std::string(), std::uint32_t resultMore = 0) {
        TuiBlock b{};
        b.turnId = turnId;
        b.id = makeStr(id);
        b.batchId = makeStr(batchId);
        b.kind = kind;
        b.status = status;
        b.flags =
            kind == TuiBlockKind_Tool ? (TuiBlockFlag_Collapsible | TuiBlockFlag_DefaultOpen) : 0;
        b.durationMs = durationMs;
        b.tokensIn = tokensIn;
        b.tokensOut = tokensOut;
        if (meta != nullptr)
            b.meta = makeStr(meta);
        if (title != nullptr)
            b.title = makeStr(title);
        b.body = makeStr(body);
        // The RESULT pair transport: parallel key/value arrays plus the
        // packed fallback string, all copied by the upsert during the call -
        // the locals here never outlive it.
        std::vector<String> keys;
        std::vector<String> values;
        for (const auto& kv : resultPairs) {
            keys.push_back(makeStr(kv.first));
            values.push_back(makeStr(kv.second));
        }
        b.resultKeys = keys.empty() ? nullptr : keys.data();
        b.resultValues = values.empty() ? nullptr : values.data();
        b.resultCount = static_cast<std::uint32_t>(keys.size());
        b.resultRaw = makeStr(resultRaw);
        b.resultMore = resultMore;
        tuiBlockUpsert(&handle, &b);
    }

    /// The message kinds a replayed exchange has: a `User` message opens a new
    /// turn with its query block, an `Assistant` message hangs a content block
    /// off the newest turn, and a `FinalAnswer` becomes the turn's Answer block.
    enum class Msg { User, Assistant, FinalAnswer };

    void add(Msg type, const std::string& text) {
        if (type == Msg::User) {
            const std::int64_t id = ++turnSeq;
            upsertTurn(id, TuiTurnState_Done, 1);
            upsertBlock(id, "query", std::to_string(id) + "#q", TuiBlockKind_Prose,
                        TuiBlockStatus_None, "user", text);
            return;
        }
        if (turnSeq == 0) { // an answer with no turn yet: open one, the way a
            turnSeq = 1;    // replayed answer-only thread has
            upsertTurn(turnSeq, TuiTurnState_Done, 1);
        }
        const std::int64_t id = liveTurnId != 0 ? liveTurnId : turnSeq;
        const int batch = liveTurnId != 0 ? liveBatch : 0;
        if (type == Msg::FinalAnswer) {
            upsertBlock(id, "answer", std::to_string(id) + "#" + std::to_string(batch),
                        TuiBlockKind_Answer, TuiBlockStatus_None, nullptr, text);
            return;
        }
        upsertBlock(id, "content#" + std::to_string(batch),
                    std::to_string(id) + "#" + std::to_string(batch), TuiBlockKind_Prose,
                    TuiBlockStatus_None, "assistant", text);
    }

    /// A live run's start: the turn upsert (Running) plus the query block.
    void runStart(const std::string& query) {
        const std::int64_t id = ++turnSeq;
        liveTurnId = id;
        liveBatch = 0;
        upsertTurn(id, TuiTurnState_Running, 0);
        upsertBlock(id, "query", std::to_string(id) + "#q", TuiBlockKind_Prose, TuiBlockStatus_None,
                    "user", query);
    }

    /// One stream tick: the batch's content-so-far block, same id every tick so
    /// the store merges in place.
    void updateStream(const std::string& text) {
        if (liveTurnId == 0)
            return;
        upsertBlock(liveTurnId, "content#" + std::to_string(liveBatch),
                    std::to_string(liveTurnId) + "#" + std::to_string(liveBatch),
                    TuiBlockKind_Prose, TuiBlockStatus_None, "assistant", text);
    }

    /// D's stop with no tool call in flight: the turn goes Interrupted.
    void runStop() {
        if (liveTurnId == 0)
            return;
        upsertTurn(liveTurnId, TuiTurnState_Interrupted, 0);
        liveTurnId = 0;
    }

    /// One live step's merged card (call + result): the tool's name is the
    /// title, the result summary the meta - and the RESULT
    /// transport: the summary feeds the packed fallback line when the card
    /// carries no pairs, the pairs ride the per-pair render when they do.
    /// The card joins the LIVE batch - the wire's shape: a response's calls
    /// share its batch. The id is the call's identity, unique per seeder.
    void tool(const std::string& name, const std::string& body, int status,
              const std::string& meta = std::string(), std::int64_t durationMs = 0,
              const std::vector<std::pair<std::string, std::string>>& resultPairs = {},
              std::uint32_t resultMore = 0) {
        tool(batchId(), name, body, status, meta, durationMs, resultPairs, resultMore);
    }

    /// The card at an explicit batch: `batch` is the exact "<turnId>#<batch>"
    /// the wire's upsert carries, so a scene can seed any wire shape.
    void tool(const std::string& batch, const std::string& name, const std::string& body,
              int status, const std::string& meta = std::string(), std::int64_t durationMs = 0,
              const std::vector<std::pair<std::string, std::string>>& resultPairs = {},
              std::uint32_t resultMore = 0) {
        if (liveTurnId == 0)
            return;
        upsertBlock(liveTurnId, "tool#" + std::to_string(++callSeq), batch, TuiBlockKind_Tool,
                    status, meta.empty() ? nullptr : meta.c_str(), body, name.c_str(), durationMs,
                    0, 0, resultPairs, meta, resultMore);
    }

    /// The batch id helpers: "<turnId>#<batchIndex>" for a response batch, the
    /// turn-level "<turnId>#q" for the query/notice rows.
    std::string batchId() const {
        return std::to_string(liveTurnId) + "#" + std::to_string(liveBatch);
    }
    std::string queryBatchId() const { return std::to_string(liveTurnId) + "#q"; }

    /// The next response batch: the wire's batch counter advances once per
    /// response, so the next response's content/calls share the new batch.
    void nextBatch() { ++liveBatch; }

    /// The live batch's content block (the response's content-so-far).
    void content(const std::string& text) {
        if (liveTurnId == 0)
            return;
        upsertBlock(liveTurnId, "content#" + std::to_string(liveBatch), batchId(),
                    TuiBlockKind_Prose, TuiBlockStatus_None, "assistant", text);
    }

    /// A notice line: its own block on the query batch.
    void notice(const std::string& text) {
        upsertBlock(liveTurnId, "notice#" + std::to_string(++noticeSeq), queryBatchId(),
                    TuiBlockKind_Notice, TuiBlockStatus_None, nullptr, text);
    }
};

} // namespace typed_seed
