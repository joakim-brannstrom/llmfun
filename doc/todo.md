- integrate internet search, searchxng
- change tools so not all are listed via "tool_calls" but rather that there is one basic tool to ask for available tools
- add Z3 as a tool
- add a simple expression execution so the llm can call a tool with e.g. `1+3*4` to calculate 
- add trace logging support to file
- Max timeout when using -p
- deep research
- add a specific /code analyze mode to update a plan/code_analysis.md
- move choosing a model to the menu
- look at what claude code is doing with .claude/rules and CLAUDE.md
- change the behavior of loading a .llmfun.yaml from the current directory if it exist to only doing it if the project is trusted
- before writing a file check that the file isn't inside a tree of symlinks from the workarea and up
- a memory file that is written should have a date when it was updated. Even better if each section in it that is changed have a date. This is to enhance the LLM's capability to reason about the "age" of the data if it gets conflicting data from different sources.
- add more @safe tags
- workarea is not allowed to be a symlink. Security reasons
- the self improvement of how to use tools should always be injected directly after the system prompt on startup if the chat is empty
- replace "format!" with interpolated strings when possible
- replace the grep tool with an internal implementation, to reduce the dependency on the OS
- make sound notification configurable in yaml via config.d
- merge the tools listDirectory, removeFile, checksumFile etc to one tool which take a command and then a key/value json string, which is the arguments. This approach should reduce the number of tools that is needed and thus the system prompt. Do a similar thing for sandbox.
- when summarizing the agent context add all messages, excluding the system prompt, to the RAG in one large document under the topic "agent_context". Protect this topic. This should then also be added to the summarized context that all details are saved in the RAG under this topic. Each time an agent is summarized or /new is called the topic is either replaced or deleted.
- modify imgui_markdown to render code blocks with Text so we get line break in them. That fix the side scrolling issue and ugly rendering on long lines.
- Each message that is processed should have their processing time recorded and other useful statistics such as tokens/s, total tokens etc.

# ui
- change the background color for the input field to dark grey
- add UI where an agent can query the user for input.
- make it easy to create a simple, straight pipeline from the TUI as a chat message.
- log messages sent to the TUI should have the severity so the heading can be colored differently
- add a tab which show a live stream of tool use
- add a menu item to open a separate window to show more detailed statistic about LLM such as latency, token use in total etc
- add a menu item to navigate the RAG database
- taskDone is displayed incorrectly when reading the database. Should be FinalAnswer

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
- there should be a mode where the pipeline execute all tasks and then optionally ask the user for input
- there should be something like /plan update, which goes through the pipeline but with other steering prompt such that the LLM understand that it should fix things in the system design and implementation plan.
- use the memory system for transporting "code/implementation.md" between agents. It should be a checksum of the implementation_plan.md

# rag
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
- If a skill-related bug prevents agent startup, users can set `disableSkills: true` in their YAML config to bypass all skill logic. This is the primary rollback mechanism.
- Glob-Triggered Skill Activation (P2)
- `allowed-tools` Permission Bypass (P2)
- skills should be part of the metrics that are collected. How often they are loaded etc
- it should be possible to mark a skill, in the frontmatter, that it should not be shown in the system prompt (xml manifest). Only available for load on demand. But there need to be a way for the model to know there are "hidden tools" that it somehow can request the "frontmatter" for. If possible avoid creating a new tool for this but maybe that is required. Consider different design alternatives.
- all skills should be added to the primary rag upon startup. Removed skills should be removed. But only if the RAG is not in-memory because otherwise it slows down startup. This is because the LLM do not always find the relevant information when it uses knowledge-retrieval because it is inside e.g. a skill's reference.
- when copying scripts also set the executable bit
