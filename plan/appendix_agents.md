# Appendix: Sub-Agent Runtime — Design Discussion Notes

Status: discussion notes only — no code written yet.
Date: 2026-09-17.
Scope: a flexible sub-agent system for llmfun, meant to loosen the static pipeline for `/plan`, `/code`, and skills-based orchestration.

Motivating example: a skill declares "you are the coordinator; spawn a sub-agent to write a system design; spawn a sub-agent to review it; when both are done, report completion". The coordinator observes the agents over a message board, then declares the skill completed.

---

## 1. What this actually is, in known terms

The proposed design is an **actor runtime for agents** (Erlang/OTP, Akka, Microsoft Orleans semantics) plus a bit of a **blackboard system** (Hearsay-II, BB1):

- per-agent mailboxes, activation on message arrival (Orleans virtual actors: activate on message, single-threaded turns, idle deactivation);
- one-to-one / broadcast / retained messages, delayed delivery;
- supervision: spawn, lifetime limits, terminate (Erlang/OTP supervision trees);
- a shared message board (blackboard) for observation and discovery;
- a coordinator + workers pattern (orchestrator-worker, Magentic-One).

This is a proven pattern space with known failure modes; the semantics have standard answers that can be copied instead of invented. The codebase already uses actor patterns internally:

- the dialogue worker is a mailbox thread (`DiJob`/`DiDrain`);
- the MCP server is a `std.concurrency` actor (with a documented unbounded-mailbox hole);
- the pipeline's `onAgentComplete` is already an event-driven completion loop over agents;
- `AgentExecutionPool` + completion callbacks + per-node context are the seed of the runtime.

The sub-agent runtime is largely the generalization of that machinery.

## 2. What's missing — the semantics (the hard part)

The message bus is the easy part. The hard part is completion, waiting, killing, budgets, and failure.

1. **Result contract.** "When they both say they are done" is the most fragile sentence. An LLM saying "done" is not a fact. Define: `taskDone(summary)` IS the result; the runtime wraps it into a `result` message to the parent with status `ok | failed | killed | budget-exhausted | timeout`. On non-ok, include partial output + transcript path. Generalize the existing `PipelineAgent` retry nudge ("You stopped without calling pipelineOutput") to sub-agents so a missing result is recoverable. This contract makes joins, cleanup, and TUI tractable.

2. **Wait/join must be a runtime primitive.** The coordinator needs `waitFor([a, b], all, timeout)` that SUSPENDS it (zero LLM turns) and wakes it with aggregated results. Polling the board burns tokens and re-reasons on every partial notification. Requires a new outcome in `Agent.runToCompletion` (e.g. suspended/waiting). Cross-cutting change: the terminal-status contract is consumed by app_agent, the pool, memory consolidation, and the pipeline — audit all consumers.

3. **Quiescence and deadlock detection.** When nothing is runnable and everything waits, someone must notice: A waits for a killed B; A and B wait on each other; a message that will never arrive. With a single-threaded scheduler this is easy: zero runnable agents + zero pending timers = quiescent -> escalate ("all idle" / "suspected deadlock: A<->B"). Give every wait a default timeout.

4. **Lifecycle & supervision rules.** States: created -> running -> idle -> waiting (blocked) -> paused -> terminated(completed/failed/killed/budget/timeout). Defaults: parent termination cascades to descendants (no orphans); every child exit notifies the parent, even when killed; a parent may terminate its own descendants; the user may do anything except terminate the main agent; the main agent is the session root. "Stopped" (resumable) and "killed" are different states.

5. **Control plane vs data plane.** Freeze/kill/pause/inject are runtime commands, NOT bus messages. A freeze command traveling through the frozen bus deadlocks the UI. Freeze: stop new activations + message delivery + timers; let the in-flight LLM call finish (a streaming HTTP call cannot be cleanly preempted); keep TUI/control live. Kill is cooperative, takes effect at the next step boundary (reuse the interrupt delegate mechanism). Document: "kill" = stopped between steps, not instant.

6. **Message schema & delivery semantics.** Needed: id, type (task/result/note/question/event), correlation id, timestamp, wake flag, TTL, size caps. Source must be runtime-stamped — never trust a payload's claimed sender. Delivery: exactly-once into an in-process mailbox, FIFO per sender, global order = scheduler log order. Document these guarantees.

7. **Broadcast activation is a stampede.** Make broadcast default to non-activating (a board post / latched status read on next activation); explicit nudge-all only if needed; scope broadcasts (subtree vs all).

8. **"Persistent message" is state, not a message.** Last-value-wins retention, never activates anyone, separate from the mailbox queue. Better: the runtime maintains the status card automatically (state, current task, budget left, last activity) — this covers discovery and feeds the TUI. Rename so state and signals never get conflated.

9. **Budgets/cost model.** Hierarchical budgets (parent allocation vs global pool — pick one); soft limit (nudge "wrap up") + hard limit (kill with status); a global per-query/session cap; fan-out limits: max depth, max children per parent, max total agents (fork-bomb protection). Multi-agent is multiplicative token cost (Anthropic's multi-agent write-up reports roughly an order of magnitude more tokens than single-agent chat — verify before quoting). Budgets are a seatbelt, not polish.

10. **Messages are untrusted input.** A sub-agent that read a hostile file can emit instructions to the coordinator via a result message. Treat message content as data with provenance; never allow impersonation of user/system; keep `from` runtime-stamped; mark user-injected messages with a distinct source.

11. **Files for data, messages for control.** Artifacts (design docs) land in the workarea; result messages carry path + summary. Small contexts, no message-size bombs. Accept the concurrent-edit hazard in v1 (document it; per-agent scratch dirs later).

12. **Determinism, testing, observability.** Single scheduler thread owning all runtime state; activation/completion serialized. Start sequential (bounded pool later, mirroring the pipeline's completion-callback model). Append-only event log (JSONL per session) as ground truth: every message, activation, state change, kill, budget event -> TUI feed, post-mortems, replay. Tests with fake `LlmRequester`: spawn/spawn/join/assert; kill cascade; budget kill; freeze gating; quiescence; broadcast non-activation. Without this, multi-agent code cannot be shipped, only hoped for.

Half-built pieces to generalize: the `pipelineOutput` retry loop is the proto result contract; `setPipelineContext(PipelineControlContext)` is the seam pattern for a per-agent runtime context (`setAgentRuntimeContext(AgentRuntimeContext)`), so spawn/send tools know caller identity and budget.

## 3. Prior art (reading list)

Caveat: from training knowledge, not verified against current docs — check each project's current state before copying.

- **Erlang/OTP**: supervision trees, links/exit signals, selective receive, bounded mailboxes, "let it crash". Copy: exit notifications to parents, cascade kill, mailbox size limits as a feature.
- **Microsoft Orleans**: virtual actors — activation on first message, single-threaded turns, idle deactivation. The activation model maps 1:1.
- **AutoGen (Microsoft)**: the 0.4 rewrite re-architected around an asynchronous, event-driven actor core (message passing, topics/subscriptions, RPC) with chat-level abstractions on top. Closest production analog; study the design and the migration pain.
- **Anthropic multi-agent research system**: orchestrator + parallel subagents, artifacts on the filesystem, condensed summaries returned; heavy emphasis on token cost and eval-driven design. Constrain communication; measure against a single-agent baseline.
- **Claude Code subagents (Task tool)**: synchronous delegation — one-shot subagent, isolated context, single result. No bus, no peer chat. Build this first; it covers most delegation needs at a fraction of the complexity.
- **LangGraph**: stateful graph orchestration with checkpointing, interrupts for human-in-the-loop, dynamic fan-out. The valuable "un-rigid" features are resumability + human interrupts.
- **Classic blackboard systems (Hearsay-II, BB1)**: boards need a control shell; activation policy is where the complexity lives.

## 4. Staged plan

**Phase 0 (paper):** semantics spec — lifecycle state machine; message schema; result contract; control ops and granularity; budget model; fan-out caps; acceptance scenario (writer+reviewer skill; kill-all; budget-kill). This is the doc that keeps the project out of the swamp.

**Phase 1 (headless core):** runtime module (`llm.agent_runtime`), single-thread scheduler, mailboxes, activation, exit notifications, cascade kill, soft+hard budgets, quiescence detection, event log. Agent tools: `spawnAgent(role, task, limits, oneShot)`, `sendMessage(to, kind, text)`, `waitFor(ids, mode, timeout)`, `terminateAgent(id)`, `listAgents()`. No TUI; `/debug` log + fake-LLM tests. Acceptance: writer+reviewer skill runs end-to-end headless, deterministically.

**Phase 2 (TUI):** agent tree panel (reuse the chat tab's left slot, which already hosts the pipeline panel), focused agent transcript (read-only first), pause/resume/kill per agent, global freeze + kill-all, user injection (queued when frozen). Snapshot-based rendering like sessions; stream only the focused agent. No tiled per-agent windows — screen space doesn't scale and it is the most expensive C++ work in the three-layer stack.

**Phase 3 (extras):** broadcast/status slots, delayed messages (min delay, max outstanding timers, paused during freeze), parallel execution pool, per-agent model config, replay/debug tooling.

**Cut from v1 explicitly:** persistence/resume across restarts (separate feature; pipeline resume is its own todo item), remote/distributed agents, capability-based routing, priority scheduling, per-agent chat input, preemption of running LLM calls.

## 5. Final honest points

- Core motivation is right: the pipeline encodes workflow in D code, so every tweak is a rebuild. Moving orchestration into skill text + runtime primitives is a real flexibility win and matches several existing todo.md items.
- The pipeline's rigidity is also its virtue: deterministic, cheap (no orchestration tokens), debuggable. Keep it for fixed flows; use sub-agents where judgment is needed. Converge later, not now.
- Validate value with the smallest slice: writer + reviewer first. If that doesn't beat a single agent at acceptable cost, no amount of freeze buttons or message boards will save the design.
- The three refinements, if nothing else: control plane separate from the bus; artifacts in files, not messages; completion via runtime joins, not board observation. The board is for humans and discovery, not for coordination logic.

Next step: turn the Phase 0 semantics spec into `doc/agent_runtime.md` and iterate before writing any code.
