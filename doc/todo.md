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
- add support in config file to configure keepalive for a server
- bad LLM's are unable to correctly call taskDone. detect when there are 5 such queries about calling it after each other and it follows a simple pattern. Then stop if the pattern detects it.
- the summary should start by removing all taskDone tool calls

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
- Add a function so the LLM can request a file+line from the database and get the text chunk.

# Prompt
