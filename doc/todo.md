- FeedbackEngine should use an agent name when it request data
- integrate internet search, searchxng
- change @Function to also take a descripton of parameters
- change tools so not all are listed via "tool_calls" but rather that there is one basic tool to ask for available tools
- add Z3 as a tool
- add a simple expression execution so the llm can call a tool with e.g. `1+3*4` to calculate 
- make ctrl+c able to interrupt a http request
- add trace logging support to file
- Max timeout when using -p
- deep research
- add autocomplete
- add a specific /code analyze mode to update a plan/code_analysis.md
- support AGENT.md
- write a system design review template and an implementation plan review template. The pipeline agents are complaining
- maybe write the context use as part of the summary in the tree node chat
- integrate imgui_markdown
- move choosing a model to the menu
- add back history save
- look at what claude code is doing with .claude/rules and CLAUDE.md
- change the behavior of loading a .llmfun.json from the current directory if it exist to only doing it if the project is trusted
- before writing a file check that the file isn't inside a tree of symlinks from the workarea and up
- a memory file that is written should have a date when it was updated. Even better if each section in it that is changed have a date. This is to enhance the LLM's capability to reason about the "age" of the data if it gets conflicting data from different sources.
- add a /fixcomments <filename> that goes through filename and updates all comments to be relevant to the current code. Those that seem to not match the code should be flagged.
- add more @safe tags
- workarea is not allowed to be a symlink. Security reasons
- the 'answer' parameter in taskDone is almost a summary of everything between it and the previous user query. Maybe the summary algorithm should be updated to only keep final answers?
- the self improvement of how to use tools should always be injected directly after the system prompt on startup if the chat is empty
- editFile remove and replace is hard for an LLM to use incrementally because it changes the line numbers. Start by requiring that "content" contains the first line of the text that is to be removed.
- if llmfun/data/scratch do not exist history file is loaded relative to the llmfun binary
- replace "format!" with interpolated strings when possible
- in tool calls and the response there is an excessive amount of escaping of slash
- replace the grep tool with an internal implementation, to reduce the dependency on the OS
- editFileByMarker, "replace" mode is probably bugged and is keeping the old content so it basically work as append. If the LLM gives marker "abc" and then the content "foo\nbar\smurf" it expectes abc to be replaced by "foo" and the next two lines to be replaced by "bar" and "smurf".
- the edit... should probably all be converted to using editFileInMemory because all the edits can be reduced down to editing lines. This would reduce the code duplication in io.d

- createEmbedder must use ModelPool. It is RAII so it ensures that models are deallocated when the pool is destroyed and enable reuse of an already loaded model

- make sound notification configurable in json via config.d

# ui
- change the background color for the input field to dark grey
- show what pipeline agents are doing in a window
- stream visualization of the current message
- add UI where an agent can query the user for input.
- make it easy to create a simple, straight pipeline from the TUI as a chat message.
- log messages sent to the TUI should have the severity so the heading can be colored differently
- add a tab which show a live stream of tool use
- add a menu item to open a separate window to show more detailed statistic about LLM such as latency, token use in total etc
- add a menu item to navigate the RAG database
- taskDone is displayed incorrectly when reading the database. Should be FinalAnswer
- send a "clear pipeline" when starting a new pipeline task

# pipeline
- must support resuming where a pipeline was last interrupted
- loading prompts from files
    - with fallback to the general agent prompt.
- make it possible to configure in more detail how /code and /plan should work. It can still be a pretty hard coded pipeline but:
1. plan: the review step should be configurable with how many there are and for each one defined it should be possible to configure the prompt.
2. code: same for the review step as for plan.

## plan
- planner
    - Change chain to. First system_design -> criticies plan -> improve -> implementation_guide -> criticies plan -> improve -> done
    - an agent that compare the system design with implementation to find deviations/contradictions

- need a mode when I update the system design and/or implementation_plan. It should then use another type of prompt.

- need to be restructured. First it should analyze the source code to understand the project. This should be written to a file in plan/. Then that is used by the system design step.
- there should be a plan execute
- each implementation task should be written to its own file with enough information for the task to be finished. The current design force the LLM to read the whole `implementation_plan.md` before it can start on a task.
- live agent status update in a new imgui window while it is working
- there should be a mode where the pipeline execute all tasks and then optionally ask the user for input
- there should be something like /plan update, which goes through the pipeline but with other steering prompt such that the LLM understand that it should fix things in the system design and implementation plan.

# rag
- memories should automatically be synchronized to the RAG so they are always searchable
- Add a warning when the DB is wiped. Need to add migration in the future.
- use an actual regulator such as PID och kalman filter for the token window instead of the primitive "5 times and fixed step +/-"
- a tui to inspect the rag DB such as what files are in it, the chunks etc. Look at the GUI for https://github.com/MrDoe/OpenCodeRAG
- add treesitter and grammar for at least c/c++/python
- add a description for a document to the db. It is an optional field
- add a "chunker" for a diff
- add builtin pdf -> text -> rag
- add builtin image -> text -> rag. This should probably be stored as "image path", "description".

# skills
- Consider adding a `skill.load` metric event (tracked by `MetricMonitor`) to identify most-used skills. Deferred to P2 — requires designing the metric schema.
- If a skill-related bug prevents agent startup, users can set `"disableSkills": true` in their JSON config to bypass all skill logic. This is the primary rollback mechanism.
- Glob-Triggered Skill Activation (P2)
- `allowed-tools` Permission Bypass (P2)
- the check if a skill should be copied should also check the version and loadSkill should have a flag "overwrite". So if the skill match but is newer than the one in sandbox it should overwrite the sandbox.
- skills should be part of the metrics that are collected. How often they are loaded etc
- when copying/overwriting a skill it must not only check the name but also the version
- if copyRecurse fail then the whole skill must be returned to the agent. This happens in the ase where there are no workarea.

# memory
## Workflow Improvements
- D. Automatic Template Loading
**Current state:** `update_memory` template must be explicitly fetched
**Proposed change:** When memory operations are triggered, automatically load the template:

When the reflection gate activates (pre-taskDone):
1. Auto-fetch `getThinkingTemplate('update_memory')` as reference
2. Follow the structured workflow in the template

## Phase 4: Feedback Loop

- H. Memory Usage Tracking
**Proposed change:** Add a mechanism to track whether memories are actually being used:

After reading a memory at session start, note in the conversation:
"Relevant prior knowledge from [topic]: [brief summary]"
This creates visible evidence that memories are being consulted.


- I. Self-Correction Mechanism
**Proposed change:** When I catch myself about to make a mistake that's documented in memory, explicitly acknowledge it:

"Memory notes that [pitfall] — avoiding that approach."
This reinforces the value of the memory system.
