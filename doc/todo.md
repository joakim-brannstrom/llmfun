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

- createEmbedder must use ModelPool. It is RAII so it ensures that models are deallocated when the pool is destroyed and enable reuse of an already loaded model

- make sound notification configurable in json via config.d

FeedbackEngine. When it triggers, such as a tool reaching a high enough threshold it should trigger a self improvement loop in the AI where it is forced to study why the tool use failed, come up with how to improve and write down as a memory for the tool. If there already exist a memory for the tool then it should be read, inspected, see if it helps correct the tool use. If not, improve or rewrite the memory.
    - This should probably execute as a separate agent that inspect how the tool was used such that it do not interrupt the current agents work and pollute the context with reasoning about how to improve the tool use.
    - Important that it do not trigger often. There must be a memory between sessions. Maybe a simple one such as keeping track of how many failed tool calls there where. If it was 20, and the improvered executed then it shouldn't execute again until 25 and 30min have elapsed. If it instead goes down to 0 the count is reset. The count has a min threshold of 10.

# pipeline
- must support resuming where a pipeline was last interrupted
- loading prompts from files
    - with fallback to the general agent prompt.
- planner
    - Change chain to. First system_design -> criticies plan -> improve -> implementation_guide -> criticies plan -> improve -> done
    - an agent that compare the system design with implementation to find deviations/contradictions

- planner: need a mode when I update the system design and/or implementation_plan. It should then use another type of prompt.

- planner need to be restructured. First it should analyze the source code to understand the project. This should be written to a file in plan/. Then that is used by the system design step.
- planner, there should be a plan execute

# rag
- Add a warning when the DB is wiped. Need to add migration in the future.
- memories should automatically be added to the RAG so they are always searchable
