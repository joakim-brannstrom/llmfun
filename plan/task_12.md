## Task 12: C++ view - the body segment

Priority: P0. Dependencies: Task 11.

### Description
Draw a batch's content as a header-less segment: the body directly, at the batch indent, with no
header measure, no hit box, no menu, no copy chip and no `expanded` of its own.

### Changes Made
- `cpp_tui/views/thread.cpp:562-600` (`drawRow`, edit): a segment branch before the header path.

```cpp
// A body segment (DD-7, I-11): no header, so no measure, no InvisibleButton, no menu, no copy
// chip and no `expanded` read. The body is the row, at the batch indent (the same pad2 the
// batch's cards take), and the renderer owns its height.
if (desc.bodySegment) {
    ImGui::Indent(m.pad2);
    ui::Rect body;
    body.x = ImGui::GetCursorScreenPos().x;
    body.y = ImGui::GetCursorScreenPos().y;
    body.w = std::max(0.0f, ImGui::GetContentRegionAvail().x);
    body.h = 0.0f;               // the renderer measures what it draws
    body.cells = m.textGrid;
    thread.renderBlockBody(index, b, body);   // markdown, tables, selection: the one path
    ImGui::Unindent(m.pad2);
    return true;
}
```

### Verification
- [ ] `make test_blocks test_streaming > build.log 2>&1` clean.
- [ ] A segment draws no header text, no disclosure mark, no `Copy` chip, and does not take the
      focus on a click (its area is body text, not a row hit box).
- [ ] The pixel profile keeps mark-and-copy: selecting text inside a segment is rich_md's own
      selection (`renderBodyText`), unchanged.
- [ ] `measureBlockHeader` is never called for a segment (assert by a counter in the test, or by
      the row height: the segment's drawn height equals the body's, not `m.row`).
- [ ] The body indent equals a card's (`m.pad2`), so a batch's content and cards share one rail.
- [ ] `test_streaming.cpp:359-370` (`streamBodyRow`, a predicate scan) still finds the segment.
- [ ] Regression: the overview's regression command passes.

### Notes
- FR-16's "reach the text through the owner" is task_09's `copyable`/`blockText` pair: the
  segment itself is not a copy target, and the grid gains no selection (the terminal owns
  `Ctrl+C`).
- An *empty* body never reaches here: the projection dropped it (task_08).

Plan pointers:
- DD-7, I-11, FR-16, C10, §7.3.3.
- Anchors were verified at plan time; re-verify before each edit.
