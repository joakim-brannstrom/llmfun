# 4
In the thread view at minimal mode I am seeing this for a running turn:

| YOU
> WORK - 60 steps (this one keep on increasing)
| YOU
<my query>

I would have expected something like this:

| YOU
<my query>
> WORK - 60 steps (increase for each command executed)
[edit_file] (current command that is either being generated or executing, when done folded into WORK)

# 5
In the thread view at maximum mode I am seeing this for a running turn:

....
> Thinking - 1 tool call - <time> <thinking summary>
| TOOL write_memory
write_memory(...)
...
RESULT error:....

I do not want a summary of the thinking part.
The thinking should be open in maximal mode. I shouldn't have to click on it to see what the model is thinking.

# 6
I see [NR] at the beginning of my queries. That was never asked for
| YOU
[NR] <my query>

# 6
The Send button do not change to Stop when a turn is running.

# 7
A tool chip I is rendered like this:

| + TOOL execute_command - <time>
execute_command(arg1=..
arg2=..
arg3=...
RESULT

I expected this:
| TOOL execute_command - <time>
arg1=..
arg2=..
arg3=...
RESULT

There is no reason to repeat the tool call name. The + do not add anything useful.

Note that not all tools render like this. request_compression for example, which have only one argument, is rendered very ugly with everything on one line.

# 8
The tool calls in the thread view are really badly rendered now.
This got worse after the supposed fix to the thread view rendering of pipeline agents.

It now looks like this:

| YOU
<my query>
| TOOL edit_file
....
...
| ANSWER - <time>
<answer>

| WORK - 102 steps
| AGENT
> Thinking - 102 tool calls ...
| * TOOL get_memory_tops
  get_memory_topics()
.....

Yes all 102 tool calls are rendered like that. This is totally broken.
