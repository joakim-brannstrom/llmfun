#include "tui_api.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

String makeStr(const char* s) { return String{s, std::strlen(s)}; }
String makeStr(std::string s) { return String{s.data(), s.size()}; };

int main(int argc, char** argv) {
    // Headless smoke mode: --frames N (or --smoke = --frames 30) renders
    // exactly N frames then exits 0, so CI can exercise the session panel
    // and the action queue without a human. Without the argument the loop
    // below runs until the user quits.
    int maxFrames = -1;
    // v4 mode flags: --tui / --gui are mutually exclusive. The standalone
    // default is the terminal backend so the headless --frames/--smoke
    // dry-runs never attempt a GUI window.
    bool tuiFlag = false;
    bool guiFlag = false;
    // Test-only fixture: how many chat messages the demo seeds. The render
    // path never reads it, so a capture stays comparable across builds.
    int seedMessages = 300;
    // Test-only fixture for the composer states (task 3): --busy starts with a
    // run active (Send shows Stop). Write-once before the first frame; the
    // render path never reads it, so a capture stays comparable across builds.
    bool busyFlag = false;
    // Test-only fixture for the role headers (task 4): --seed-roles swaps the
    // demo thread for one message per role (see the seeding below).
    bool seedRoles = false;
    // Test-only fixture for the thread states (task 8): --empty-thread clears
    // the demo thread so the binary starts on what a brand-new session shows
    // (wordmark, sentence, starter actions). The render path never reads it.
    bool emptyThreadFlag = false;
    // Test-only fixture for the rail (task 7): --seed-sessions N replaces the
    // three-session smoke snapshot with N generated sessions (titles
    // "Session 00..N-1", every third one carrying a preview) so a capture can
    // overflow the panel and show the scrollbar. The flag being present is what
    // selects the generated snapshot - N is honoured exactly, including 0..2.
    // The render path never reads it.
    int seedSessions = 0;
    bool seedSessionsGiven = false;
    // Both numeric flags share this bound: the value is narrowed to int, so an
    // out-of-range input must be rejected instead of truncating silently.
    constexpr long MaxNumericFlag = 1000000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--busy") == 0) {
            busyFlag = true;
        } else if (std::strcmp(argv[i], "--seed-roles") == 0) {
            seedRoles = true;
        } else if (std::strcmp(argv[i], "--empty-thread") == 0) {
            emptyThreadFlag = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long n = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || n < 0 || n > MaxNumericFlag) {
                std::fprintf(stderr, "error: --frames requires an integer in [0, %ld]\n",
                             MaxNumericFlag);
                return 2;
            }
            maxFrames = static_cast<int>(n);
        } else if (std::strcmp(argv[i], "--smoke") == 0) {
            maxFrames = 30;
        } else if (std::strcmp(argv[i], "--tui") == 0) {
            tuiFlag = true;
        } else if (std::strcmp(argv[i], "--gui") == 0) {
            guiFlag = true;
        } else if (std::strcmp(argv[i], "--seed-messages") == 0 && i + 1 < argc &&
                   argv[i + 1][0] != '-') {
            char* end = nullptr;
            long n = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || n < 0 || n > MaxNumericFlag) {
                std::fprintf(stderr, "error: --seed-messages requires an integer in [0, %ld]\n",
                             MaxNumericFlag);
                return 2;
            }
            seedMessages = static_cast<int>(n);
        } else if (std::strcmp(argv[i], "--seed-sessions") == 0 && i + 1 < argc &&
                   argv[i + 1][0] != '-') {
            char* end = nullptr;
            long n = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || n < 0 || n > MaxNumericFlag) {
                std::fprintf(stderr, "error: --seed-sessions requires an integer in [0, %ld]\n",
                             MaxNumericFlag);
                return 2;
            }
            seedSessions = static_cast<int>(n);
            seedSessionsGiven = true;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--tui | --gui] [--frames N | --smoke] [--busy] "
                         "[--seed-roles] [--empty-thread] "
                         "[--seed-messages N] [--seed-sessions N]\n",
                         argv[0]);
            return 2;
        }
    }
    if (tuiFlag && guiFlag) {
        std::fprintf(stderr, "error: --tui and --gui are mutually exclusive\n");
        return 2;
    }
    const TuiBackendMode mode = guiFlag ? TuiBackendMode_Gui : TuiBackendMode_Tui;

    TuiState* state = tuiCreateState();
    if (!state) {
        std::fprintf(stderr, "Failed to create TUI state.\n");
        return 1;
    }

    // Before tuiInit: the filename is captured into io.IniFilename when the
    // ImGui context is created.
    tuiSetIniFilename(state, makeStr("imgui2.ini"));

    if (tuiInit(state, mode) != 0) {
        std::fputs(guiFlag ? "Failed to initialize GUI backend.\n"
                           : "Failed to initialize TUI. Check terminal compatibility.\n",
                   stderr);
        String err = tuiLastError();
        if (err.data && err.len > 0) {
            std::fprintf(stderr, "Error: %.*s\n", (int)err.len, err.data);
            String_Free(err);
        }
        tuiDestroyState(state); // backend-less state: safe teardown
        return 1;
    }

    tuiSetLogging(state, true);
    // The status row is seeded by tuiCreateState (model chip "none", no numbers
    // yet); the D app re-sends its own status on the first render tick.

    // Composer fixture (task 3): start with a run already active, applied
    // before the first frame, so a capture shows the same composer state a user
    // reaches when uiAgentBusy arrives (Send has become Stop). The draft state
    // needs real typing and is driven through tmux in
    // tools/capture_composer.sh.
    if (busyFlag)
        tuiReadyStatus(state, 0);

    // Standalone max-width cap via LLMFUN_TUI_MAX_WIDTH (env var only, no
    // CLI flag). Debugging aid: the user-facing config is the D YAML
    // tui.maxWidth. Tolerant parse: unset, empty, non-numeric, or negative
    // values are ignored (0 = unlimited); positive sub-40 caps are raised
    // to 40 by the C API (the TUI's minimum render width).
    if (const char* mwEnv = std::getenv("LLMFUN_TUI_MAX_WIDTH")) {
        char* end = nullptr;
        long mw = std::strtol(mwEnv, &end, 10);
        if (end != mwEnv && *end == '\0' && mw >= 0) {
            // Mirror validateConfig's upper bound so the int narrowing below
            // is well-defined (the standalone path bypasses D validation).
            if (mw > 10000)
                mw = 10000;
            tuiSetMaxWidth(state, static_cast<int>(mw));
        }
    }

    // Headless smoke seed: session sidebar snapshot so the panel renders
    // rows and the action poll can be exercised. Inbound strings are copied
    // by tuiSetSessionList, so the literals may go out of scope.
    if (maxFrames >= 0) {
        if (seedSessionsGiven) {
            // --seed-sessions N: exactly N rows (0..N-1). N above the panel's
            // height overflows it, so a capture shows the rail's scrollbar and
            // the rows below the window height.
            // The id/title/preview strings live in the vectors below and the
            // SessionItems point at their buffers: `tuiSetSessionList` copies
            // during the call, so nothing may dangle before it runs.
            std::vector<SessionItem> wide;
            std::vector<std::string> ids;
            std::vector<std::string> titles;
            std::vector<std::string> previews;
            const auto n = static_cast<std::size_t>(seedSessions);
            wide.reserve(n);
            ids.reserve(n);
            titles.reserve(n);
            previews.reserve(n);
            for (int i = 0; i < seedSessions; ++i) {
                char id[32];
                char title[32];
                std::snprintf(id, sizeof id, "seed-%03d-session", i);
                std::snprintf(title, sizeof title, "Session %02d", i);
                ids.emplace_back(id);
                titles.emplace_back(title);
                previews.emplace_back(i % 3 == 0 ? "preview of the first message" : "");
            }
            // The v5 fields (task 24): a timestamp per row so the rail can group
            // them, spread over five hours each so a 30-row fixture crosses the
            // TODAY / YESTERDAY / PREVIOUS 7 DAYS / OLDER boundaries. One clock read
            // for the whole fixture, so the spacing is exactly five hours.
            const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
            for (int i = 0; i < seedSessions; ++i) {
                const auto k = static_cast<std::size_t>(i);
                wide.push_back(SessionItem{makeStr(ids[k].c_str()), makeStr(titles[k].c_str()),
                                           makeStr(previews[k].c_str()), k, i == 0 ? 1 : 0,
                                           nowMs - static_cast<std::int64_t>(i) * 5LL * 3'600'000LL,
                                           /*flags=*/0,
                                           /*reserved=*/0});
            }
            tuiSetSessionList(state, wide.data(), wide.size());
        } else {
            std::array<SessionItem, 3> sessions = {{
                {makeStr("20260815-100000-aaaa"), makeStr("Alpha session"),
                 makeStr("first user preview"), 3, 1},
                {makeStr("20260815-100000-bbbb"), makeStr("Beta session"),
                 makeStr("second user preview"), 5, 0},
                {makeStr("20260815-100000-cccc"), makeStr("Gamma session"), makeStr(""), 0, 0},
            }};
            tuiSetSessionList(state, sessions.data(), sessions.size());
        }
    }

    // Seeded demo thread: --seed-messages N (default 300) controls how many
    // messages the fixture adds; the render path reads nothing else from here.
    // Typed seeding: one Done turn per message - the query cadence a user
    // Prose block in the turn's query batch, the final-answer cadence an Answer
    // block, everything else an assistant content block in the turn's batch.
    const auto addTypedTurn = [&state](std::int64_t turnId, int kind, const std::string& body,
                                       const std::string& meta, const char* blockId,
                                       const char* batchSuffix) {
        const std::string turn = std::to_string(turnId);
        TuiTurn t{};
        t.id = turnId;
        t.state = TuiTurnState_Done;
        t.messageCount = 1;
        tuiTurnUpsert(state, &t);
        TuiBlock b{};
        b.turnId = turnId;
        b.id = makeStr(blockId);
        b.batchId = makeStr(turn + batchSuffix);
        b.kind = kind;
        b.status = TuiBlockStatus_None;
        b.durationMs = 1200;
        b.meta = makeStr(meta);
        b.body = makeStr(body);
        tuiBlockUpsert(state, &b);
    };
    for (int i = 0; i < seedMessages; ++i) {
        const std::int64_t turnId = i + 1;
        std::string text{u8"**hello**\nthis is *some* much\n# Heading\nlonger text '😜' 'ö' "
                         u8"\n\n'⚠️'\n✅\n\n***\n\n"};
        text.append(std::to_string(i));

        if (i % 10 == 0) {
            addTypedTurn(turnId, TuiBlockKind_Prose, text, "user", "query", "#q");
        } else if (i % 15 == 0) {
            addTypedTurn(turnId, TuiBlockKind_Answer, text, "", "answer", "#0");
        } else {
            addTypedTurn(turnId, TuiBlockKind_Prose, text, "assistant", "content#0", "#0");
        }
    }

    // The submit fixture's turn counter: the query echoes get ids the seeds
    // never use (the seeds end at seedMessages + 9).
    int submitTurnSeq = 0;

    // The code-block fixture: an assistant prose block whose body is markdown
    // with a fenced json block.
    addTypedTurn(seedMessages + 1, TuiBlockKind_Prose,
                 "# Heading1\nsmurf\n[a link](to somewhere)\n\n## Heading2\nsmurf\n\n### "
                 "Heading 3\nsmurf\n\nSome content "
                 "before.\n**bold**\n*italic*\n\n```json\n{\"key\": \"value\", \"nested\": [1, "
                 "2, 3]}\n{\"another\": true}\n```\n\nText after code block.\n",
                 "assistant", "content#0", "#0");

    // Long-line fixtures: a body with no fence (the renderer's automatic line
    // breaking) and one fenced as a code block (verbatim drawing).
    std::string longLine;
    for (int i = 0; i < 31; ++i)
        longLine += "long cat, long cat, long cat, long cat, long cat, long cat, long cat, "
                    "long cat, long cat, long cat,\n";
    longLine += "long cat, long cat\n";
    addTypedTurn(seedMessages + 2, TuiBlockKind_Prose, longLine, "assistant", "content#0", "#0");
    addTypedTurn(seedMessages + 3, TuiBlockKind_Prose, "```\n" + longLine + "```\n", "assistant",
                 "content#0", "#0");

    // Test-only fixture for the role headers (task 4): --seed-roles replaces the
    // demo thread with one block per kind, one Done turn each, in the order the
    // thread groups them. It gives a capture a screen where every block kind is
    // visible. Write-once before the first frame; the render path never reads
    // the flag.
    if (seedRoles) {
        tuiThreadClear(state); // replace the demo thread, never augment it
        const std::int64_t base = seedMessages + 4;
        addTypedTurn(base + 0, TuiBlockKind_Answer, "Answer body.", "", "answer", "#0");
        addTypedTurn(base + 1, TuiBlockKind_Prose, "Ask for the roles.", "user", "query", "#q");
        addTypedTurn(base + 2, TuiBlockKind_Prose, "Assistant body.", "assistant", "content#0",
                     "#0");
        // The tool pair: one call (Pending) and its result (Ok), as separate
        // turns here so every header shows.
        addTypedTurn(base + 3, TuiBlockKind_Tool, "Tool call body.", "readFile", "call#0", "#0");
        addTypedTurn(base + 4, TuiBlockKind_Tool, "Tool result body.", "readFile", "result#0",
                     "#0");
        addTypedTurn(base + 5, TuiBlockKind_Notice, "System body.", "", "notice#0", "#0");
    }

    // Test-only fixture for the thread states (task 8): an empty thread is what
    // a brand-new session shows, so the capture needs the demo seeding gone.
    if (emptyThreadFlag)
        tuiThreadClear(state);

    auto addPipelineAgents = [&state]() {
        for (int ii = 0; ii < 40; ++ii) {
            const auto i = ii % 16;
            std::string agentId{"agent "};
            agentId.append(std::to_string(i));

            std::string content{"hello"};
            std::string reasoning{u8"**hello**\nthis is *some* much\n# Heading\nlonger text '😜' "
                                  u8"'ö' \n'⚠️'\n✅\n \n***\n\n"};
            reasoning.append(std::to_string(i));

            std::string role{"assistant"};
            std::string finishReason = (ii % 3 == 0) ? "work" : "done";

            tuiPipelineAgentUpdate(state, makeStr(agentId),
                                   PipelineChatMessage{makeStr(content), makeStr(reasoning),
                                                       makeStr(role), makeStr(finishReason)});
        }

        for (int i = 7; i < 9; ++i) {
            std::string agentId{"agent "};
            agentId.append(std::to_string(i));
            tuiPipelineAgentDone(state, makeStr(agentId));
        }
    };

    auto pipelineShow = true;
    auto pipelineShowAt = std::chrono::system_clock::now() + std::chrono::seconds{1};

    // Frame-count exit for --frames N; the body is identical to the
    // interactive loop.
    int frame = 0;
    while (true) {
        if (maxFrames >= 0 && frame >= maxFrames)
            break;

        tuiBackendNewFrame(state);

        if (tuiRender(state) == 0)
            break;

        if (pipelineShow && std::chrono::system_clock::now() > pipelineShowAt) {
            pipelineShow = false;
            addPipelineAgents();
        }

        if (tuiIsSubmitReady(state) != 0) {
            String query_ = tuiGetSubmitQuery(state);
            std::string query;
            if (query_.data && query_.len > 0) {
                query = std::string{query_.data, query_.len};
            }
            String_Free(query_);

            if (!query.empty()) {
                if (query.rfind("/pipeline show", 0) == 0) {
                    addPipelineAgents();
                } else if (query.rfind("/pipeline clear", 0) == 0) {
                    tuiPipelineClear(state);
                } else if (query == "/busy" || query == "/ready") {
                    // Task-3 fixture (same spirit as /pipeline show|clear): the
                    // standalone harness has no agent, so /busy and /ready drive
                    // the run state the D app sets through uiAgentBusy/Ready --
                    // this is what turns Send into Stop and back.
                    tuiReadyStatus(state, query == "/busy" ? 0 : 1);
                } else {
                    std::string text = query;
                    std::string summary = text.size() > 30 ? text.substr(0, 30) : text;
                    // The submitted query echoes into the thread the way the
                    // live emitter echoes it: a Running turn with a user Prose
                    // block in the turn's query batch.
                    submitTurnSeq++;
                    TuiTurn t{};
                    t.id = static_cast<std::int64_t>(seedMessages + 100 + submitTurnSeq);
                    t.state = TuiTurnState_Running;
                    tuiTurnUpsert(state, &t);
                    TuiBlock b{};
                    b.turnId = t.id;
                    b.id = makeStr("query");
                    b.batchId = makeStr(std::to_string(t.id) + "#q");
                    b.kind = TuiBlockKind_Prose;
                    b.meta = makeStr("user");
                    b.body = makeStr(text);
                    tuiBlockUpsert(state, &b);
                    std::string log = std::to_string(text.size());
                    // The stamped path: every production record crosses the ABI with a
                    // sequence number, so the Raw Log can account for what a drop removed.
                    TuiLogRecord logRecord{};
                    logRecord.level = TuiLogLevel_Info;
                    logRecord.message = String{summary.c_str(), summary.size()};
                    logRecord.detail = String{log.c_str(), log.size()};
                    tuiAddLogRecord(state, &logRecord);
                }
            }
            tuiResetSubmit(state);
        }

        // Task-3 fixture: consume a Stop-button request exactly once per frame,
        // like the D UI loop does (tuiTakeCancel -> stopAgent + "Stopping"
        // status). The harness has no agent to stop, so it records the request
        // in the Log tab (stderr would corrupt the live ncurses screen) and
        // shows the same status text.
        if (tuiTakeCancel(state) != 0) {
            // The same status the D loop shows on a stop: waiting, with the step named.
            TuiRuntimeStatus stopping;
            stopping.phase = 4; /* waiting */
            stopping.detail = makeStr("Stopping agent");
            tuiSetRuntimeStatus(state, &stopping);
            // The stamped path: every production record crosses the ABI with a sequence
            // number, so the Raw Log can account for what a drop removed.
            TuiLogRecord cancelRecord{};
            cancelRecord.level = TuiLogLevel_Info;
            cancelRecord.message = makeStr("cancel requested");
            cancelRecord.detail = makeStr("Send/Stop button");
            tuiAddLogRecord(state, &cancelRecord);
        }

        tuiBackendRender(state);
        ++frame;
    }

    if (maxFrames >= 0) {
        // Headless smoke: exercise the session action poll. Nothing was
        // clicked in the dry-run, so the queue must be empty and the pop
        // must return the None sentinel with NULL strings (String_Free is
        // a no-op on them). This proves tuiIsSessionActionReady /
        // tuiGetSessionAction work end-to-end in the executable.
        if (tuiIsSessionActionReady(state) != 0) {
            std::fprintf(stderr, "error: expected empty session action queue\n");
            tuiDestroyState(state);
            return 1;
        }
        SessionAction action = tuiGetSessionAction(state);
        String_Free(action.sessionId);
        String_Free(action.title);
        if (action.type != TuiSessionAction_None) {
            std::fprintf(stderr, "error: expected TuiSessionAction_None sentinel\n");
            tuiDestroyState(state);
            return 1;
        }
        std::printf("smoke ok: rendered %d frames, session action queue empty (None sentinel)\n",
                    frame);
    }

    tuiDestroyState(state);
    return 0;
}
