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
- memory directory in ~/.local/share/llmfun should always be created
- add a /fixcomments <filename> that goes through filename and updates all comments to be relevant to the current code. Those that seem to not match the code should be flagged.

- createEmbedder must use ModelPool. It is RAII so it ensures that models are deallocated when the pool is destroyed and enable reuse of an already loaded model

- make sound notification configurable in json via config.d

FeedbackEngine. When it triggers, such as a tool reaching a high enough threshold it should trigger a self improvement loop in the AI where it is forced to study why the tool use failed, come up with how to improve and write down as a memory for the tool. If there already exist a memory for the tool then it should be read, inspected, see if it helps correct the tool use. If not, improve or rewrite the memory.
    - This should probably execute as a separate agent that inspect how the tool was used such that it do not interrupt the current agents work and pollute the context with reasoning about how to improve the tool use.
    - Important that it do not trigger often. There must be a memory between sessions. Maybe a simple one such as keeping track of how many failed tool calls there where. If it was 20, and the improvered executed then it shouldn't execute again until 25 and 30min have elapsed. If it instead goes down to 0 the count is reset. The count has a min threshold of 10.

# ui
- change the background color for the input field to dark grey
- support streaming of queries
- show what pipeline agents are doing in a window
- stream visualization of the current message
- add UI where an agent can query the user for input.

# pipeline
- must support resuming where a pipeline was last interrupted
- loading prompts from files
    - with fallback to the general agent prompt.

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
- the output from the query functions should contain the database the chunk came from and then this should be part of the Result that the LLM see so it know which database to search further in.

# rag
- Add a warning when the DB is wiped. Need to add migration in the future.
- memories should automatically be added to the RAG so they are always searchable
- use an actual regulator such as PID och kalman filter for the token window instead of the primitive "5 times and fixed step +/-"
- a tui to inspect the rag DB such as what files are in it, the chunks etc. Look at the GUI for https://github.com/MrDoe/OpenCodeRAG
- add treesitter and grammar for at least c/c++/python
- add a description for a document to the db. It is an optional field
- add a "chunker" for a diff
- add builtin pdf -> text -> rag
- add builtin image -> text -> rag. This should probably be stored as "image path", "description".
- add a date to all sources and then present it to the LLM so it can reason on the age of a source.

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
