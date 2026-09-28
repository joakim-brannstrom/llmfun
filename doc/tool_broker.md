# Design Document: Tool Broker

---

## 1. Overview & Purpose

The tool broker is the component between the model and the global tool
registry: it decides which tools each request actually carries. The registry
holds every registered tool definition; sending all of them on every request is
wrong on three counts.

- **Prompt budget and focus.** Every schema attached to a request is tokens the
  model reads on every turn. A raw registry dump grows the request and dilutes
  tool choice - the model does measurably worse picking from a large, noisy
  menu.
- **Cache stability.** All three supported backends cache prompt prefixes and
  reuse them on the next request, so a tools array that changes shape between
  requests re-processes work it could have reused - a real cost amplifier on
  long conversations (section 3).
- **Per-agent visibility.** Different agents need different tools at different
  times; a single static array cannot express that.

The broker's job: keep the per-request `tools` array small, focused per agent,
and byte-stable between change points. Everything below serves that job.

---

## 2. What the Broker Does

Pure functions over the registry plus small per-agent state, so the whole
mechanism is unit-testable without a live model.

- **Pool construction.** The global registry is filtered by configuration
  (include/exclude name filters, plus a never-hide list that survives the
  filters) into the *pool* - the set a broker-managed agent may ever see.
- **Selection.** The per-request array is built in a fixed order: untagged
  (always-on) tools first in registry order, never-hidden tools forced into the
  head, then activated tools in activation order; deduplicated by name. The
  pool is never sorted or reshuffled - identical state emits byte-identical
  arrays (pinned by a unittest).
- **Activation.** An agent sees the always-on head by default; tagged tools
  activate on demand, by tag or by single tool, and activation is sticky.
  Unknown tags and names are no-ops at this layer; the instructive message is
  the discovery layer's. A per-tag activation counter feeds the discovery
  tool's presentation cap (frequency then name).
- **Instructive refusal at the dispatch site.** A tool in the pool but not
  visible to the calling agent (tagged, never activated) refuses with
  "discover tools with `listToolTags`" so the model recovers by itself; tools
  excluded by configuration or absent from the registry fall through to the
  generic executors' refusals.
- **Compression-time pruning.** At compression change points, activated tools
  with no tool call in the intact pre-compression chat are pruned from the
  activation list - conservative: only structured tool-call names count,
  summary text and prose never do, and the whole chat is scanned. The apply
  gate belongs to the caller: prune only when compression actually rewrote
  history.

---

## 3. Tools-Array Stability and Prompt Caching

Part of the broker's design, and the reason its selection is shaped the way it
is.

**Why byte-stability is needed.** Every time the `tools` array changes, the
backend re-processes the prompt from wherever `tools` lands - a prompt cache
miss, which costs money and time. On tools-first backends the whole prompt is
re-processed; on a long conversation that is the worst kind of churn. Hence the
contract: the serialized array is **byte-identical between change points**,
changing only when the tool set actually changes - agent activation, compression -
never between them.

**Why churn frequency, not placement.** Where `tools` lands in each backend's
assembled prompt varies by backend and by served model template, and is neither
observable nor controllable from the client. The one lever the client owns is
*how often* the array changes. Byte-stability bounds churn frequency under every
outcome; the per-bust cost stays backend-dependent (table below).

**Why top-level body key order is irrelevant.** The position of the `tools` key
in the JSON body does not matter: llama.cpp re-renders the request through the
model's chat template, and the remote APIs assemble the prompt server-side. Only
the rendered token stream reaches the cache.

| Backend | Where `tools` lands | Effect of a tools-array change |
| :--- | :--- | :--- |
| **llama.cpp** (local inference) | At the front - rendered through the model's chat template; the shipped Qwen and DeepSeek template families embed tools into the opening system block | Whole prompt re-processed from token 0. Partial reuse is still native: the server reuses the longest common token prefix (1-token granularity, on by default), and per-request reuse counts are already surfaced to the agent. |
| **OpenAI-style remote APIs** | Last - the documented cacheable prefix is messages, images, files, then tools | Only the tools tail is re-processed; the conversation prefix survives. Caching is automatic; prompts under the 1,024-token minimum are not cached, and reuse accrues in 128-token increments. |
| **DeepSeek hosted API** | Undocumented; the vendored DeepSeek template renders tools into the system prompt, so tools-early is the standing assumption | Assume the whole prefix busts until observed otherwise. Caching is always on, quantized to 64-token blocks. |

For the local backend the served template is not observable from the repo, so
tools-first is the standing worst-case assumption - and a realistic one, since
the shipped Qwen and DeepSeek templates both embed tools first.

---

## 4. Design Rationale: Why This Shape

- **Selection order is contractual, not cosmetic.** The never-sorted,
  deterministic emission is what makes byte-identical arrays - and therefore the
  reusable prefix - possible at all; any "helpful" normalization would reshuffle
  the prefix and turn every change into a full bust.
- **Rejected alternative: trimming or reordering the array per request** to
  "help" the cache. A per-turn trim is itself a change point: each turn's array
  differs from the previous request's, so every turn busts the cached prefix from
  wherever `tools` lands - the whole prompt on tools-first backends - and
  re-processing a long conversation every turn is far more costly than the
  stable-array design, for no gain.

---

## 5. Known Gaps & Maintenance

- **Cache-hit reporting is dropped on the floor:** the OpenAI-style and DeepSeek
  usage payloads carry per-request cache-hit fields, and llama.cpp exposes a
  reuse count that the agent already consumes - but the remote-API fields are
  not parsed, so the tools-array metric cannot be cross-checked against reported
  cache hits. Actionable later if a per-provider cross-check is wanted.
- **Assumptions to re-check:** the OpenAI prefix-order sentence and the
  DeepSeek tools-placement (both from provider documentation), and the
  tools-first template assumption for the local backend. If a provider changes
  its template or documented order, revisit the cost column in section 3 - the
  broker design itself (section 2) is unaffected.
