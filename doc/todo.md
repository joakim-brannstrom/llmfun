- FeedbackEngine should use an agent name when it request data
- integrate internet search, searchxng
- change @Function to also take a descripton of parameters
- change tools so not all are listed via "tool_calls" but rather that there is one basic tool to ask for available tools
- add Z3 as a tool
- add a simple expression execution so the llm can call a tool with e.g. `1+3*4` to calculate 
- make ctrl+c able to interrupt a http request
- add trace logging support to file
- models should be possible to have multiple of
- Max timeout when using -p
- readLines number of lines should be configurable
- deep research

- createEmbedder must use ModelPool. It is RAII so it ensures that models are deallocated when the pool is destroyed and enable reuse of an already loaded model

- make sound notification configurable in json via config.d

FeedbackEngine. When it triggers, such as a tool reaching a high enough threshold it should trigger a self improvement loop in the AI where it is forced to study why the tool use failed, come up with how to improve and write down as a memory for the tool. If there already exist a memory for the tool then it should be read, inspected, see if it helps correct the tool use. If not, improve or rewrite the memory.
    - This should probably execute as a separate agent that inspect how the tool was used such that it do not interrupt the current agents work and pollute the context with reasoning about how to improve the tool use.
    - Important that it do not trigger often. There must be a memory between sessions. Maybe a simple one such as keeping track of how many failed tool calls there where. If it was 20, and the improvered executed then it shouldn't execute again until 25 and 30min have elapsed. If it instead goes down to 0 the count is reset. The count has a min threshold of 10.

# pipeline
- planner: system design and implementation agents should use the standard system prompt. The "task to execute" should be injected in the chat as a user query.
    - when it is restructured to a graph then change 

- must support resuming where a pipeline was last interrupted
- loading prompts from files
    - with fallback to the general agent prompt.
- planner
    - Change chain to. First system_design -> criticies plan -> improve -> implementation_guide -> criticies plan -> improve -> done
    - an agent that compare the system design with implementation to find deviations/contradictions

- planner: need a mode when I update the system design and/or implementation_plan. It should then use another type of prompt.

# rag
- Add option to drop all unknown
- Add a warning when the DB is wiped. Need to add migration in the future.
- Add option to drop all files that aren't found
- queryBestMatch fungerar inte när sökordet innehåller "smurf-bar"
- remove unknown, change it to "topic" instead

- must check the write permissions of the directory before trying to create the database.

# Prompt

In llmfun/source/llm/plan.d there is an implementation of a pipeline. The problem with it is that it is only two stages. I want you to suggest how it can be improved. Maybe a study step first? That the LLM should study the source code and write a memory about it. But first look if there is a memory for this source code.

In the rag implementation in llmfun/source/llm/rag.d and database.d there is a category called unknown. I want that change to being a topic instead. A topic is not related to a file but rather just that, a topic about something. A topic is then related to a "Document" which is indexed, in the same way that Unknown is indexed. So the change is basically changing unknown to a Topic, changing the database to be able to accomodate a topic and the tool call in tool_call/rag.d
