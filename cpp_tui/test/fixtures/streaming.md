# Streaming v2 — the live answer

A markdown-heavy answer, used as the *source* of the streaming fixture in
`./test_streaming`. The test feeds it to the thread one growing prefix at a time
(300 updates, as a real model stream arrives), so every construct below is seen
in every intermediate state: a half-written list item, a heading without its
text, a fence without its closing line.

## What the fixture is for

- it is prose, so the body is drawn by the markdown renderer, not by the
  verbatim path a tool transcript takes;
- it is long enough to wrap at the reference width, which is what makes the
  height a number worth measuring;
- it holds one of each construct the renderer has a branch for.

## The constructs

1. a paragraph with `inline code`, **bold** and a [link](https://example.com);
2. a bulleted list;
3. a fenced block;
4. a block quote;
5. a table, which the text grid draws as a grid (`ui/table.*`, task 37) and the
   pixel profile draws with rich_md's own renderer.

> A quote is a bar plus a paragraph. The caret must not land on the bar.

```cpp
int live_answer(int tick) {
    // a streamed body is re-rendered on every update
    return tick * 2;
}
```

| Tick | Text | Caret |
| ---- | ---- | ----- |
| 1 | `Streaming v2` | on |
| 2 | `Streaming v2 - the live answer` | on |
| 3 | the whole document | off |

### The last line

The caret belongs at the end of this line, because this is the line the model is
still writing when the fixture's 300th update arrives.
