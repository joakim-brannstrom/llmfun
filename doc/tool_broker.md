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
- **Selection.** The per-request array is built in a fixed order: ungrouped
  (alwaysOn) tools first in registry order, never-hidden tools forced into the
  head, then activated tools in activation order; deduplicated by name. The
  pool is never sorted or reshuffled - identical state emits byte-identical
  arrays (pinned by a unittest).
- **Activation.** An agent sees the always-on head by default; tools in hidden
  groups activate on demand, by group name or by single tool, and activation is
  sticky. Unknown group names and tool names are no-ops at this layer; the
  instructive message is the discovery layer's. A per-group activation counter
  (keyed by group name) feeds the discovery tool's presentation cap (frequency
  then name).
- **Instructive refusal at the dispatch site.** A tool in the pool but not
  visible to the calling agent (a member of a hidden group, never activated)
  refuses with "discover tools with `listToolTags`" so the model recovers by
  itself; tools excluded by configuration or absent from the registry fall
  through to the generic executors' refusals.
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

## 5. Configuration & Migration

The broker is configured per model through the `toolBroker` block in
`config/*.yaml`; a fully worked example ships as `config/example.yaml`.

```yaml
toolBroker:
  enabled: false                       # kill switch, per model overridable
  groups:
    workarea:
      description: "Files in the agent workarea: read/write/list/search"
      tools: [writeFile, readFile, editFile, listDirectory, grepFiles]
    pipeline:
      description: "Completion tools for agents running in a pipeline"
      tools: [pipelineOutput]
  hiddenTags: [workarea]               # only these groups' members start hidden
  alwaysOn: [taskDone, listToolTags]   # tool names OR group names
  neverHideTools: [taskDone]           # pool-membership axis, unchanged
```

**Resolution.** The effective config for a request is built in layers, each
later layer overriding the previous one per field: the built-in struct defaults,
then the global `toolBroker` block, then the model's own `toolBroker` block
(inside its `codeModels` entry), then the agent's broker adjust hook - an
internal integration point pipelines use to pin tools programmatically (it
mutates the freshly resolved config on first use of a model and on every model
switch; the mutation is not written back into `LlmConfig`). Within a block,
scalar and list fields replace wholesale, while `groups` merges per entry by
group name: a model group entry named like a global one replaces that group's
description and tool list and keeps the other groups.

**Consequence of the per-field override.** Groups are not "everything hidden by
default": only the group names listed in `hiddenTags` hide anything. A NEW group
inherited from the global block under a model override is therefore visible
unless the model's own `hiddenTags` names it - and a model `hiddenTags` replaces
the global list wholesale (no merge), so extending it means re-listing the
groups the model wants hidden. Override what you need.

**Migration (clean break; there is no deprecation shim - pre-1.0 project).**

- A config that never had a `toolBroker` section keeps working unchanged:
  tools in no group are alwaysOn before and after this change, so nothing is
  hidden and the broker is inert (`enabled` defaults to false).
- A config using `toolTagDescriptions` must move to `groups` entries whose
  `description` feeds discovery; the old key is dropped with a targeted
  startup warning pointing at the `groups` migration.
- A config that already had a `toolBroker` block changes in three ways:
  1. The broker starts DISABLED if it relied on the old `enabled = true`
     default rather than an explicit `enabled: true`. This is the one visible
     behavior change - check it first when upgrading.
  2. `toolTagDescriptions` is dropped (the startup warning above).
  3. The old tag categorization silently disappears unless the config migrates
     to `groups` (there is no shim).

**Mixed `alwaysOn` semantics.** `alwaysOn` entries may be tool names OR group
names in one array: a group name un-hides the group's whole membership, a tool
name un-hides just that tool.

**All-members-alwaysOn corner.** A hidden group whose members are ALL in
`alwaysOn` still shows up in `listToolTags`, and activating it is a no-op:
every member was already visible, so activation changes nothing.

---

## 6. Known Gaps & Maintenance

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
