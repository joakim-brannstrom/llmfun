# 1
classifyEditError

# 2
In the raw log when a log message is evicted because the log is full it should not count as a dropped message.
It should only count as an evicted message, which is shown at the top in dull color.

A dropped message is one which was overwritten in the buffer on the D side.
- I say that on the assumption that there is a mutex so while the buffer is being drained to the UI log message "halt" the rest of the application.

# 3
There is still a duplicate ID problem in the agent tab

> coder * running - 12:00
151k tok - 100 tok/s
tool: execute_command

When I hover with the mouse on tool: execute_command imgui complain. It only happens when all three lines are printed. If only the two at the top are printed it is OK.
So the "151k..." and "tool:.." lines use duplicate IDs.

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

# 10
RESULT is a {....}
It should be "unpacked" one level IF it is json on the D side and sent to the c++ side as key/value (probably two separate vectors with String).
If it isn't a JSON then just send it as it is.

# 11
There should be "short lived messages", ephemeral, that live for like 1 minute. A time sent from the D side.
This should be used for messages from the D side that are "information".
When the timer has triggered, they are removed.

They are currently shown as SYSTEM messages.

# 12
Have you looked at how pipeline messages are rendered?
They are wrong

This is what it looks like (note the > should be a down arrow for open folds)

> WORK 432 steps     (open fold)
| AGENT
> Thinking - 216 tool calls       (open fold)
The thinking text......

| + TOOL get_memory_topics
{<tool call arguments as JSON>}
RESULT ....

I only see the latest thinking trace. As soon as a new one start streaming it is overwritten.
All tool calls are visible.
I would have expected multiple Thinking traces with their associated tool calls.
I also expected the stream to be at the bottom and not the top.
It is also not possible to fold a Thinking trace.

# 14
I no longer see the result, final report, of running a pipeline

# 15
views/thread.cpp
renderThreadBody is too large. Break it down into separate inline functions that are called in the method

# 16
the currently live tool call is not being printed at the bottom. I saw it being printed like 4 "| YOU" up.

# 18
In maximal mode only the latest thinking+tool calls should be shown. Both while streaming and then when the tools are executed.
When the last tool is no longer live (being streamed or executing. Basically all tools have responded with a result) it should be folded

The TOOL that is part of a "thinking trace" are not part of the Thinking fold.
What I see:

> Thinking ....
> TOOL read_file        (I cannot write down arrows so by TOOL is a down arrow)
  path=...
  RESULT ...

What I expected, when thinking is folded, is this:
> Thinking ....

And when I open Thinking I expect this:

> Thinking ....           (I cannot make a down arrow so left of Thinking is a down arrow)
  thinking text about life and stuff.....
  > TOOL read_file        (I cannot write down arrows so by TOOL is a down arrow)


It is even more stupid now because the tools never fold and the "> Thinking..." is always folded.

# 19
The open "WORK" should auto-close when the turn is done

# 20
The "content" part do not seem to be rendered anymore.
Sometimes the model stop with a "finish_reason" and a "content" message. Then it gets a nudge to call task_done and do that.
But the "content" is lost. that is not OK.
It should be shown as part of the thinking fold.
