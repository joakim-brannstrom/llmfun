module llm.cli;

import std.array : appender, empty;

import my.path : Path;
import std.string : toStringz, fromStringz, endsWith;

import linenoise;

/// set up persistent history
void configLinenoise(Path historyFile = Path.init, int len = 0) {
    if (len != 0)
        linenoiseHistorySetMaxLen(len);
    if (historyFile != Path.init)
        linenoiseHistoryLoad(historyFile.toStringz);
    linenoiseSetMultiLine(0);
}

string multiLineConsole(string prompt, string contPrompt = "> ", Path historyFile = Path.init) {
    auto result = appender!string();
    bool first = true;

    scope (exit) {
        // Save history before exiting
        if (historyFile != Path.init)
            linenoiseHistorySave(historyFile.toStringz);
    };

    while (true) {
        const char* pr = first ? prompt.toStringz : contPrompt.toStringz;
        char* raw = linenoise.linenoise(pr);
        if (raw is null) { // Ctrl+D → EOF
            if (result[].length == 0)
                return null;
            break;
        }
        string line = raw.fromStringz.idup;
        linenoiseHistoryAdd(raw);
        linenoiseFree(raw);

        if (first && line.empty)
            return null;

        first = false;
        bool continuation = line.endsWith("\\");
        if (continuation)
            line = line[0 .. $ - 1];

        result.put(line);
        if (!continuation)
            return result[];
        result.put('\n');
    }

    return null;
}
