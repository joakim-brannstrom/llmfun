# Common File-Editing Patterns

## Replace a single line by marker

editFile(path="file.txt", mode="replace", marker="the line to replace", content="new line")

## Replace a block by marker (auto-count)

editFile(path="file.txt", mode="replace", marker="first line of block",
         content="line 1\nline 2\nline 3")
// Auto-derives count=3 from content. Replaces 3 lines starting at marker.

## Replace only marker line with multi-line content

editFile(path="file.txt", mode="replace", marker="the line", count=1,
         content="replacement line 1\nreplacement line 2")
// Explicit count=1 overrides auto-count. Replaces only 1 line.

## Replace a known code block

editFile(path="file.txt", mode="replace",
         searchContent="old line 1\nold line 2",
         content="new line 1\nnew line 2")

## Replace all occurrences

editFile(path="file.txt", mode="replace", searchContent="old code",
         content="new code", replaceAll=true)

## Replace by marker within a line range (large files)

editFile(path="file.txt", mode="replace", marker="target",
         scopeStart=120, scopeEnd=180, content="new code")
// Search is limited to lines 120-180 (1-based, inclusive).
## Insert before a marker

editFile(path="file.txt", mode="insert_before", marker="existing line",
         content="new line inserted before")

## Append after a line (by line number)

editFile(path="file.txt", mode="append", startLine=5,
         content="line added after line 5")

## Remove lines

editFile(path="file.txt", mode="remove", startLine=10, count=3, content="")

## Apply a unified diff

applyDiff(path="file.txt", diff="--- a/file.txt\n+++ b/file.txt\n@@ -1,3 +1,3 @@\n old\n-new\n+changed\n context")

## Write a new file

writeFile(path="newfile.txt", content="full file content here")
