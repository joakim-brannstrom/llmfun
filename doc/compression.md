# Agent-Initiated Context Compression
It is a memory‑management strategy for long‑running conversational agents. When the agent’s context window reaches a high usage threshold (e.g., 80%), the system injects a nudge message warning that forced compression is imminent and could cause uncontrolled information loss. The agent can then proactively call a dedicated `requestCompression` tool, providing a `messageToSelf` parameter: a self‑written summary containing the current task state, key decisions, open questions, constraints, and next steps. The system then compresses the context, discarding older turns, and re‑injects the summary back into the conversation (often as a system‑tagged user message). The agent uses this summary as its memory of prior events and continues seamlessly, without losing critical continuity.

**How it works:**
1. **Monitor**: The system tracks context usage as a ratio (e.g., 0.84).
2. **Nudge**: When usage exceeds a threshold (e.g., 80%), a system nudge informs the agent and urges it to compress on its own terms.
3. **Agent Action**: The agent calls `requestCompression` and writes a detailed, self‑contained summary.
4. **Compress & Inject**: The system truncates the conversation, then re‑inserts the summary with a clear indication that compression occurred and the agent should continue.
5. **Resume**: The agent reads the summary and picks up the task from where it left off.

This approach gives the agent control over what information is preserved, reducing the risk of silent, destructive truncation and improving reliability in extended tasks.
