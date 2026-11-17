# Appendix: Agent Runtime — mylib.actor suitability analysis

Status: analysis for the system design of the agent runtime (`llm.agent_runtime`).
Inputs: the Subject discussion, `plan/appendix_agents.md` (first thoughts), `plan/appendix_user_design_notes.md`, and a source-level review of `./mylib/source` plus five executable probes against the local mylib (all probes pass; see Appendix A).
Date: 2026-09-18.

---

## 0. Verdict up front

1. **mylib.actor is a good basis — for the transport and lifecycle kernel of the runtime, not for the runtime itself.** It gives you mailboxes, activation-on-message, typed dispatch, delayed messages, request/reply with timeouts, monitors/links, kill, and a working scheduler. That is exactly the "message bus" half of the design, already built, tested, and owned by us.

2. **Everything that makes this feature hard is still missing and must be built in `llm.agent_runtime`**: supervision rules, wait/join, the result contract, freeze/pause, budgets, discovery, message schema/provenance, quiescence, and the event log. mylib is not an OTP supervision framework; `link` is a crude "die together" coupling, not a supervision strategy. Do not expect to "just use the actor system" and get the feature.

3. **No mylib change is strictly required for v1.** The runtime can be built entirely on top. A few small, additive mylib changes are worth doing *if* specific spikes show the need (see §5.1): a deterministic test mode, scheduler observability, bounded mailboxes, address liveness accessors. Do not grow supervision/registry/freeze semantics into mylib.

4. **Two hard caveats, known before coding:**
   - **Kill/pause are cooperative and take effect at handler boundaries.** Probe-verified: killing a busy actor waits for the current handler to return (≈710 ms in the probe). The runtime must therefore wire the *existing* interrupt delegate into agent turns so "kill/pause/budget" can abort between LLM steps (and mid-stream, since `LlmRequester` checks the interrupt while streaming). Otherwise a "killed" agent may keep running for a full turn.
   - **Freeze/pause cannot be implemented with the library; it is a runtime delivery gate.** All deliveries and timers must go through the runtime, or freeze leaks (the library's own system messages — DownMsg, kill, reply-timeouts — bypass any gate). This is consistent with the appendix's control-plane/data-plane split, but it means discipline: the runtime must own every send/timer that a frozen agent could observe.

5. **Parallel execution is what mylib gives you** (user note: the system cannot be single-threaded). The appendix's "single scheduler thread" idea is dropped; accept it. Consequences: per-agent turns are serialized (probe-verified — same actor never runs two handlers concurrently), different agents run concurrently on the worker pool, and global ordering becomes "per-mailbox FIFO + runtime event log". Determinism for tests comes from controlled message flows and a fake `LlmRequester`, not from a single-threaded scheduler.

6. **Scope check (is it too complex?):** The full wish list (bus + board + persistent slots + freeze + per-agent windows) is a lot, but the complexity lives in *semantics*, not in plumbing. With mylib the plumbing is basically free. Concrete recommendation: v1 = spawn / initial task / result / wait / kill / hard budgets / event log (everything else deferred). That smallest slice covers the coordinator-skill use case and exercises all the risky semantics.

---

## 1. The target, restated as the acceptance frame

From the Subject + appendices, the runtime must support this end-to-end story:

- A skill tells the main agent: "You are the coordinator. Spawn a writer sub-agent; spawn a reviewer sub-agent; when both are done, report."
- Sub-agents are real agents (own prompt, own model config, own tools, own transcript), running **in parallel** with the main agent and each other.
- They coordinate over a message system: per-agent mailboxes; sender always visible; 1:1, broadcast, one retained ("persistent") slot per agent, delayed/self-ping messages.
- Agents are **activated when they have a message**; nothing runs without a message.
- The user can freeze/unfreeze everything, inject messages, inspect each agent (TUI), and kill agents; kill-all; the main agent is never terminated but can be stopped.
- Spawning agent controls the child's prompt, its lifetime (token/wall budget), and can terminate it.

User constraints (notes file): module `llm.agent_runtime`; parallel execution required; do not disturb the pipeline.

This analysis judges mylib against this frame.

---

## 2. What mylib.actor is (verified facts)

### 2.1 Architecture as built (source refs)

- **Address = mailbox.** `Address` (mailbox.d:71–213) holds four mutex-guarded queues: user messages (`Msg`: type-signature + Variant payload), system messages (`SystemMsg`: monitor/link/exit/error), delayed messages, and replies. `WeakAddress`/`StrongAddress` are handles to it. Sending to a closed/stopped address is rejected (put returns false; probe 1 #4: send-to-dead is silently dropped).
- **Message identity is the type signature** (`makeSignature` = typeid hash, common.d:178). A handler is registered per signature; payload is a `std.variant.Variant` tuple (msg.d:136–141). Keep every logical message kind a distinct struct type, or it will collide.
- **Activation-on-message scheduler.** `System` owns a `TaskPool` of worker threads plus a watcher and a shutdowner thread (system.d:313–549). Workers pop "waiting" actors and call `Actor.process` up to `maxThroughput` (default 50) messages per run; actors with no messages go to "inactive"; the watcher moves an actor back to "waiting" when `hasMessage()` (incoming, system, delayed, or replies — mailbox.d:185–194) or when a timer is near (system.d:372–423). This *is* "agents are activated when they have a message".
- **An actor with registered handlers stays alive when idle** (actor.d:599–603 only self-terminates when it has no behaviors at all). Idle agents cost a watcher poll, nothing else.
- **Delayed/self-ping**: `delayedSend` (msg.d:120–126) into a per-actor time-sorted tree (actor.d:850–870); verified firing ≈300 ms (probe 1 #2).
- **Request/reply with timeout**: `request(...).send(...).then(...)`, awaited-reply table + timeout wheel (actor.d:672–700, 810–848, 905–919). Verified (probe 1 #6, probe 2 A/B).
- **Kill/lifecycle**: `sendExit(addr, reason)` (msg.d:128–133) → `SystemExitMsg`. `kill` force-shuts even a trapping actor; `userShutdown` is trappable via custom `exitHandler`; `normal` is a no-op (probe 5 A1/A2/C). Kill of a busy actor is deferred until the current handler returns (probe 3 #2).
- **Liveness notifications**: `monitor()` → `DownMsg` on death (probe 1 #3); `linkTo()` → `ExitMsg` to the other side on death, defaulting to a cascade kill unless trapped (probe 2 D1/D2, probe 5 B).
- **Exception policy**: handler exceptions go to `exceptionHandler_`; default force-shutdown, custom handler may keep the actor alive (probe 2 E).
- **Spawn**: `System.spawn` (system.d:104–135); dynamic spawn from *inside a handler* works (probe 4: `self.homeSystem.spawn(...)`), which is exactly the "spawn tool" path. **Spawn ≠ start**: the new actor runs nothing until it receives a message — the runtime must send the initial task message (probe 4 initially failed for precisely this reason).
- **Serialization**: same-actor handlers never run concurrently (probe 3 #3); different actors run in parallel on the pool (probe 2 C).
- **Shutdown**: `System.shutdown` is kill-all (system.d:74–102): workers send `kill` to residents and drain. No graceful drain-all primitive; runtime teardown must sequence itself.

### 2.2 Version and environment

- The local `mylib/source/my/actor` tree is **byte-identical to the published `mylib` 0.0.51** in the dub cache (verified with `diff -rq`, no output). llmfun pins `mylib ~>0.0.51`. So the probe results describe exactly what llmfun would build against.
- The five probes in `/workarea/actor_probe{..5}` build against `../mylib` (path dependency) and all pass (Appendix A). Probe 4 needed a fix during this analysis (its draft never sent the child an initial message); it now passes and is the spawn-from-handler acceptance probe.

---

## 3. Requirement-by-requirement suitability

Legend: **Direct** = library feature, verified; **Partial** = primitives exist but semantics must be built or are subtly different; **Absent** = must be built in the runtime.

| # | Requirement (Subject + appendix) | mylib | Where / notes |
|---|---|---|---|
| 1 | Per-agent mailbox | Direct | `Address` queues; runtime id ↔ `WeakAddress` mapping kept by runtime |
| 2 | Sender always visible | Absent | No `from` field in `Msg`; runtime stamps `from` and never trusts payload claims |
| 3 | 1:1 send | Direct | `send()`; mailbox is a single FIFO queue (insert-back / pop-front) |
| 4 | Broadcast (one-to-all) | Absent | Runtime fan-out over registry (scoped: subtree vs all). Make "board post" non-activating (state), "nudge" activating |
| 5 | "Persistent message" (one per agent) | Absent | Runtime state slot (status card), last-value-wins; **not** a queued message |
| 6 | Delayed message / self-ping | Direct | `delayedSend`; probe 1 #2 |
| 7 | Activation on mailbox content | Direct | watcher + `hasMessage`; probe 1 #1; latency ~ms (kill-idle 50 ms, probe 3) |
| 8 | Discover live agents | Absent | Runtime registry (id, role, parent, state, budget, address). `Actor.name` exists but has no lookup |
| 9 | Freeze / unfreeze bus+agents | Absent | Runtime gate: no deliveries, no timers, no new turns; control plane stays live. Caveats in §4.1 (item 4) |
| 10 | User message injection | Absent | Runtime command; source=user; delivered through the gate |
| 11 | TUI inspect + per-agent controls + kill-all | Absent (UI work) | Runtime snapshots + streams; reuse `UiMessenger`/pipeline-stream patterns; kill-all = loop of `sendExit(kill)` |
| 12 | Spawn sub-agent tool | Direct + runtime | Actor spawn verified (probe 4); runtime wraps identity, caps, initial task message |
| 13 | Control child's prompt | Absent | Spawn parameter (role/system prompt assembled by the spawning agent; runtime may enforce a preamble) |
| 14 | Max lifetime: token / wall budget | Absent | Runtime: token accounting via `step` callback/`ServerStat`; wall via runtime timers; soft nudge + hard kill; probe 2/3 show the interrupt/step hooks exist in `Agent.runToCompletion` |
| 15 | Terminate own children | Direct + rules | `sendExit(kill)` works (untrappable); cascade/descendant rules are runtime policy |
| 16 | Main agent never terminated, can be stopped | Design choice | Keep root agent out of the kill surface; reuse the existing `stopAgent` signal for the root. See §5.3 |
| 17 | Parallel execution | Direct | worker pool (probe 2 C); per-agent serialization guaranteed (probe 3 #3) |
| 18 | Result contract ("both say they are done") | Absent | Runtime wraps completion; needs a small `Agent` seam for `taskDone` outcome (§5.3) |
| 19 | Wait/join (`waitFor`) | Absent | Runtime primitive: agent suspends by *returning from its handler*; wake is a delivered message. No LLM turns while waiting |
| 20 | Quiescence / deadlock detection | Absent | Runtime counters + wait-graph cycle check; wait timeouts mandatory |
| 21 | Lifecycle state machine (created…terminated) | Partial | mylib has internal states (waiting/active/shutdown/stopped); runtime keeps its own richer state and maps |
| 22 | Supervision rules (cascade kill, notify parent, no orphans) | Partial | Primitives: monitor (Down), link (cascade), trap-exit (probe 2 D1, probe 5 B). Policies are runtime code |
| 23 | Message schema (id/type/corr/ts/TTL/size caps) | Absent | Runtime struct; single type routed through one polymorphic struct keeps dispatch simple |
| 24 | Mailbox caps / backpressure | Absent | Queues unbounded; runtime caps its own queues; `utility/limiter` is a reusable permit-actor pattern if needed |
| 25 | Determinism / test hooks | Partial | Multi-threaded scheduler; tests must be integration-style (as mylib's own are) or use an optional pump patch (§5.1) |
| 26 | Observability / event log | Absent | Runtime JSONL event log; mylib has message counters and a `mylib_actor_trace` version for debugging |
| 27 | Untrusted content handling | Absent | Runtime: runtime-stamped `from`, user/agent/system provenance classes; content is data |

---

## 4. The holes, grouped

### 4.1 Semantics holes (the real work)

1. **Supervision/lifecycle policy.** No parent/child graph, no default rules. Decide in the runtime: parent death cascades to descendants (default on) vs orphan reparenting; every child exit notifies the parent (monitor is the tool); parent may kill its descendants; user may do anything except terminate root; "stopped" ≠ "killed".
2. **Wait/join + suspension.** The single most important primitive for the motivating skill. Implement as: a `waitFor(ids, mode, timeout)` tool that (a) records the wait in the runtime, (b) requests suspension, (c) the turn aborts cleanly at the next step boundary via the interrupt delegate. Results accumulate **in the runtime**, and only when the join condition completes does the runtime deliver one wake message. This gives zero LLM turns while waiting and avoids waking on partial results.
3. **Result contract.** `taskDone(summary)` → runtime `result` message {status: ok|failed|killed|budget|timeout; summary; artifact paths; transcript ref}. Non-ok statuses include partial output. Missing `taskDone` needs a recovery nudge (generalize the pipeline's `pipelineOutput` retry). Requires a small seam on `Agent` (completion flag/message accessor).
4. **Freeze/pause.** Runtime gate + honest semantics: "in-flight turn finishes; no new turns; no deliveries; no timers; control plane and TUI live." Two leaks to handle: (a) a message delivered just before freeze may still produce one turn after freeze — either accept ("quiesce may lag one delivered message") or add a defer-on-entry check in the handler that returns the payload to the runtime for re-delivery; (b) library system messages (DownMsg/kill) still wake actors — keep handlers for those no-op/bookkeeping only, never turn-running.
5. **Budgets & caps.** Per-agent token budget (from `step` stats), wall deadline (runtime timer), soft nudge ("wrap up") + hard kill; global session cap; fan-out caps: max depth, max children per parent, max total agents (fork-bomb protection). Multi-agent is multiplicative cost; keep the caps in v1 even if the UI is v2.
6. **Quiescence & deadlock.** With all waits carrying default timeouts, detect: zero running turns + zero pending timers + zero deliverable messages + no wait satisfied → "all idle"; if idle while waits remain unresolved, cycle-check the wait graph → "suspected deadlock A↔B" escalation.
7. **Message schema & provenance.** One `RuntimeMsg` struct: id, type (task/result/note/question/wake/control), from (runtime-stamped), to, corr id, ts, ttl, bounded body + optional structured JSON. Delivery guarantees documented: exactly-once into the runtime queue, FIFO per (sender,receiver), global order = event log. TTL checked at delivery (drop + log). Payload discipline: no interior pointers (library contract), prefer immutable strings; artifacts go to files, messages carry paths + summaries.
8. **Discovery & status.** Runtime registry is the single source of truth for liveness/state; the "message board" is a read-only projection of it (plus recent events). The board is for humans and discovery; **coordination logic uses waits/results only** (otherwise you re-argue the appendix's "completion via runtime joins, not board observation").

### 4.2 Library-level gaps (with mitigations)

9. **No deterministic test mode.** The scheduler is parallel and poll-timed; make the *runtime's* decision logic a unit-testable state machine, and test actor behavior with controlled flows (mylib's own tests poll with deadlines; probes did the same). Optional: a tiny mylib pump/step API (§5.1) if test flakiness appears.
10. **No observability into queues from outside.** Runtime keeps its own counters; optional mylib scheduler stats if quiescence detection needs a cross-check.
11. **Address lifetime hazards.** No public liveness query on `WeakAddress`; terminated actors are disposed by the scheduler. Rules: registry removes the address on `DownMsg`; never send to a dead agent from stored state; never call `Actor.shutdown/forceShutdown` from foreign threads — always `sendExit` (the state field is not synchronized).
12. **Unbounded mailboxes.** Only reachable via bugs/fan-out storms; mitigated by runtime queue caps + fan-out caps. (The `utility/limiter` actor is a reusable permit/backpressure pattern if we ever need throughput control.)
13. **No drainage primitive.** `System.shutdown` is kill-all. Runtime teardown: stop gate → kill/settle agents → drain runtime queues → then `System.shutdown`.
14. **Kill latency.** Deferred to handler boundary (probe 3). With the interrupt-delegate wiring the practical granularity is "one LLM step" (and mid-stream abort is possible). Document it as the kill/pause contract; the TUI must set expectations ("stopping…").
15. **Handler threads are pool threads.** Everything a turn touches must be thread-aware. This is the biggest *integration* risk (next section).

### 4.3 Integration holes in llmfun (independent of mylib)

16. **Shared services are not thread-safe.** `MetricMonitor` appends to a JSONL file and mutates arrays without a lock; RAG/sqlite has no internal locking; `UiMessenger` is fine (`std.concurrency.send`), and `IStreamCallback.clone()` is the established per-thread pattern, but agents' tool calls will hit RAG/monitor/files concurrently once sub-agents run in parallel. Decide per service: serialize (lock/dedicated thread), restrict tools for sub-agents, or accept documented races. This must be in the system design, not discovered in testing.
17. **Main agent model.** Root agent stays synchronous on the main thread in v1 (see §5.3); its "mailbox" is an app-level queue; `waitFor` for the root is a pumping wait loop that honors the stop/interrupt flags. Moving the root into the actor system is a later convergence, not v1.
18. **Completion seam on `Agent`.** The runtime needs the `taskDone` outcome and the ability to enforce completion (the appendix's result contract). Add a narrow, additive seam (`Agent` getter/callback) rather than reaching into tool internals.
19. **Session/artifact policy.** Sub-agent transcripts: where stored, whether compression checkpoints are wired to the dialogue/reasoning index (current note: sub-agent compressions are not session-stamped — recommend not wiring them in v1), and scratch/edit-conflict rules (accept for v1, per-agent scratch later).

### 4.4 What is *not* a hole (available or cheap)

- Spawn/monitor/link/kill/delayed/request-reply/exception policies: all probe-verified, use as-is.
- Flow control: `utility/limiter` (token permits + promises) can seed global concurrency/budget mechanics.
- UI plumbing: pipeline panel + per-agent stream messages + snapshot patterns already exist to copy.
- The interrupt delegate + step callback: already plumbed through `Agent.runToCompletion` → `LlmRequester` (mid-stream checks included).

---

## 5. What to add or modify

### 5.1 mylib.actor — keep changes small, additive, and optional

For v1: **no change required.** Recommended candidate changes, each only if a spike/need justifies:

1. **Deterministic test hook** (medium value): a public "manual pump" or single-thread step mode (`process`/`isAlive` are currently `package`), so runtime tests can step actors without sleeps. Alternative: keep integration tests. Decide after spike S3.
2. **Scheduler observability** (low/medium): expose lightweight counts (runnable/inactive/shutdown) — would let the runtime cross-check quiescence and feed the TUI. Alternative: runtime-only bookkeeping.
3. **Bounded mailbox option** (low): cap per-address queue; policy = reject-with-log at the runtime layer anyway. Only if we worry about storms.
4. **Address conveniences** (low): public `isAlive`-style check for addresses, or a `terminate(addr, reason)` helper. Nice for tooling; not load-bearing.
5. **Do NOT add**: supervision trees, registry, freeze flags, budget accounting, or message schema into mylib. mylib stays a generic actor kernel used by other projects; runtime semantics live in llm.agent_runtime.

If we do modify: additive only; release as a mylib minor bump; update llmfun's pin (or use a local path override during development). Remember llmfun builds against registry 0.0.51 today.

### 5.2 `llm.agent_runtime` — the module to build

Suggested decomposition (names indicative):

- **core / registry**: agent table (id, role, parent, state, budget, address, status card), parent/child edges, lifecycle rules, control ops (`spawn/terminate/pause/resume/freeze/unfreeze/inject/killAll`), all under one mutex (short critical sections) — the "runtime bookkeeping".
- **bus / gate**: runtime queues; delivery gate (freeze/pause), per-receiver FIFO, TTL, caps, provenance stamping, broadcast fan-out, wake policy.
- **messages**: the single `RuntimeMsg` struct + constructors (task/result/note/wake/control).
- **supervision**: cascade policies, exit notifications (monitor-based), budget enforcement (token/wall), fork-bomb caps, result contract assembly.
- **agent actor**: mylib actor wrapper that (a) receives `RuntimeMsg`, (b) runs `Agent.runToCompletion` with step/interrupt wired to budgets/kill/pause/suspend, (c) emits events + result messages. This is where "one activation = one turn" is implemented.
- **timers / quiescence**: runtime timer queue (obeying freeze) + quiescence/wait-cycle detection.
- **event log**: JSONL per session; every message, activation, state change, kill, budget event; feeds TUI and post-mortems.
- **tools seam**: `setAgentRuntimeContext(AgentRuntimeContext)` analog of `setPipelineContext`; tools: `spawnAgent`, `sendMessage`, `waitFor`, `terminateAgent`, `listAgents`, plus `taskDone` completion wiring.
- **root agent adapter**: main-agent handle (queue, stop flag, wait pump).
- **UI** (phase 2): snapshots + streams via `UiMessenger`.

### 5.3 Integration changes in llmfun

- **Agent seam**: expose completion outcome (`taskDone` message/flag) and let the runtime enforce the result contract; keep it additive.
- **Interrupt wiring**: per-turn interrupt = OR(kill|pause|budget|suspend|global stop) checks; step callback = budget/event/stream hooks.
- **Main agent (v1)**: synchronous root; `waitFor` implemented as a pump-loop (control ops + injected messages + stop flag, then sleep); freeze stops children only for a non-waiting root; `stopAgent` behavior preserved.
- **Tool policy for sub-agents**: decide the shared-service access rules (§4.3 #16); v1 simplest: read-only RAG ops + serialized monitor, or a per-agent tool filter (`ReFilter` exists).
- **TUI**: new panel/tab with agent list + focused transcript + inject input + pause/resume/kill + freeze toggle + kill-all. Start with the pipeline-panel + pipeline-stream pattern; tiled per-agent windows explicitly deferred (appendix stance holds).

---

## 6. Proposed runtime semantics (concrete, for the design doc)

- **States** (runtime, mapped onto mylib internal states): `created → running(turn) → idle → waiting(join) → paused → stopping → terminated(reason: completed|failed|killed|budget|timeout)`.
- **Activation**: `send` a `RuntimeMsg` through the gate → mylib mailbox → handler runs one turn. Spawn sends the initial `task` message; without a message nothing runs (acceptance detail from probe 4).
- **Suspend/wake**: `waitFor` records a join; turn aborts at step boundary (interrupt); on completion the runtime sends one `wake` message with aggregated results; timeout/default timeouts mandatory.
- **Kill**: set kill flag (interrupt) + `sendExit(kill)`; result message to parent (status=killed, partial output); descendants cascade per policy. Deferred up to the current step — document.
- **Pause**: gate holds deliveries for that agent; in-flight turn finishes; resume re-delivers FIFO.
- **Freeze**: global gate + timers held; control plane (freeze toggle, kill, inject, TUI) live; inject queues.
- **Budgets**: soft nudge via `addContinueMessage`; hard = kill with status=budget; global cap; fan-out caps.
- **Provenance**: system/user/agent classes; `from` always runtime-stamped; message content treated as data.
- **Artifacts**: files; messages carry path + summary; result messages bounded.

---

## 7. Staged plan deltas (vs appendix_agents.md §4)

- **Phase 0 (paper)**: keep; update the spec with: parallel-accepted architecture from this doc; kill/pause granularity contract; freeze gate rules; budget caps in v1; main-agent root adapter; integration thread-safety decisions.
- **Phase 1 (headless core)**: *larger savings than expected*: transport/activation/timers/kill/monitor come from mylib. Build: registry, gate, joins, result contract, budgets, event log, agent-actor wrapper, tools, root adapter. Acceptance: writer+reviewer skill headless (spawn → parallel turns → join → result), kill-all, budget-kill, freeze-delivery test, cascade test. Tests: fake `LlmRequester`; polling-style assertions like the probes.
- **Phase 2 (TUI)**: as appendix; reuse pipeline panel/stream plumbing; freeze/inject controls; snapshots. No tiled windows in v1.
- **Phase 3 (extras)**: broadcast/board polish, delayed utilities, per-agent model config, replay tooling; keep the appendix's explicit cuts (no cross-restart resume, no remote agents, no preemption).

---

## 8. Risks & spikes before/during Phase 1

- **S1 — kill/pause inside a long turn**: wire interrupt + verify a running sub-agent aborts between steps and mid-stream (uses existing `LlmRequester` interrupt checks). *Blocks the "user can stop/kill" story.*
- **S2 — freeze semantics**: gate + defer-on-entry decision; test "frozen: no new turns; in-flight finishes; TUI live; unfreeze resumes FIFO".
- **S3 — test determinism**: fake LLM + pool sizes 1..N; measure flakiness; decide on optional mylib pump patch.
- **S4 — shared service thread safety**: RAG/monitor/files under parallel sub-agents; choose serialize vs restrict; must be decided before Phase 1 acceptance.
- **S5 — root adapter**: non-waiting root + waiting root + stop-while-waiting; prove `/stop` and inject behavior.
- **S6 — address lifetime**: long-running session with many spawns; verify no registry leaks/dangling address use (monitor-driven cleanup).

---

## 9. Alternatives considered (and why not)

- **stdlib `std.concurrency`** (as the MCP server uses): thread-per-actor, blocking `receive`, no timers/monitors/scheduler; activation-on-message with cheap idle agents is not its model. Worse fit.
- **Bespoke runtime** (own scheduler, no mylib): maximal control/determinism, but re-implements mailboxes/timers/monitors/lifecycle (~1–2 kLOC) only to face the same semantic work afterwards. mylib is our own code and adequate; modify it instead.
- **External actor frameworks**: no compelling D option in-tree; new dependency and new failure modes; rejected.
- **mylib as-is + thin wrapper only** (no runtime semantics): rejected — reproduces known multi-agent failure modes (polling joins, ambiguous completion, no budgets).

**Conclusion: adopt mylib.actor as the substrate; build `llm.agent_runtime` above it; patch mylib only additively and only where a spike proves the need.**

---

## 10. Open decisions I need from you

1. Freeze edge-case: accept "one delivered message may still run after freeze", or build the defer-on-entry mechanism?
2. Root agent: keep synchronous root adapter for v1 (recommended), or prioritize actor-izing the main agent?
3. Sub-agent tool policy: which tools are allowed initially (esp. RAG/write access) and how to handle shared-service serialization?
4. Sub-agent transcript/indexing policy (store where; do not index compressions in v1 or stamp parent session?).
5. Tiling: confirm "focused transcript panel first, tiled per-agent windows not in v1".
6. Optional mylib additions (§5.1): which ones, if any, do you want specced now?

---

## 11. Bottom line

- **Suitable: yes — with a clearly bounded role.** mylib.actor is the mailbox/activation/lifecycle kernel; `llm.agent_runtime` is the governance layer (supervision, joins, budgets, freeze, contract, discovery, log).
- **The five things that must not be hand-waved** before coding: result contract, wait/join with suspension, freeze as a delivery gate, kill/pause granularity via the interrupt delegate, and thread-safety rules for shared services.
- **The plan gets cheaper, not riskier:** the transport half is built and verified; the remaining work is exactly the semantic half that any design would have to build anyway.
- Next step: turn this appendix + `appendix_agents.md` + the user notes into the Phase 0 semantics spec (proposed doc: `doc/agent_runtime.md`), resolving §10, then run spikes S1–S3.

---

## Appendix A — Probe evidence (rerun commands)

All probes live under `/workarea/actor_probe{1..5}` and build against `../mylib` (path dep).
Run (in the llmfun sandbox env):

```
cd /workarea/actor_probe  && timeout 60 ./actor_probe
cd /workarea/actor_probe2 && timeout 60 ./actor_probe2
cd /workarea/actor_probe3 && timeout 60 ./actor_probe3
cd /workarea/actor_probe4 && dub build && timeout 60 ./actor_probe4   # fixed 2026-09-18; needs rebuild
cd /workarea/actor_probe5 && dub build && timeout 60 ./actor_probe5   # built 2026-09-18
```

Results observed (2026-09-18):

- **P1**: activation-on-message OK; delayed send fired at 303 ms OK; kill+monitor (DownMsg) OK; send-to-dead dropped OK; link cascade double-death OK; request/reply OK.
- **P2**: async request/reply OK; reply timeout → `requestTimeout` OK; blocking handler does not stall other actors (pool 4) OK; trap-exit supervisor survives child death OK; kill cascades to 2 linked children OK; custom exceptionHandler keeps actor alive OK.
- **P3**: kill idle actor ~50 ms; kill busy actor deferred ~710 ms until handler returns (expected); same-actor serialization (no concurrent handlers) OK.
- **P4** (fixed): dynamic spawn from inside a handler OK; child→parent message via spawn-captured parent address OK; monitor of the fresh child OK. Lesson: spawn alone runs nothing — the initial task message is what starts the agent.
- **P5**: trap `userShutdown` (survives) OK; `kill` untrappable OK; graceful child exit notifies linked parent (ExitMsg), parent survives when trapping OK; `sendExit(normal)` no-op OK.
