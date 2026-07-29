/**
 * Brace balance checker for D source files.
 * Runs as a standalone D program to verify brace matching.
 *
 * Usage:
 *     ldc2 check_braces.d -of=check_braces && ./check_braces <file.d>
 *
 * Reports unbalanced braces with line numbers and context.
 */
import std.stdio;
import std.file;
import std.string;
import std.array;
import std.algorithm;
import std.conv;

struct BraceError {
    int line;
    string message;
    string context;
}

void checkSimple(string filepath) {
    auto content = readText(filepath);
    int opens = content.count('{');
    int closes = content.count('}');
    int balance = opens - closes;

    writeln("File: ", filepath);
    writeln("  Open braces:  ", opens);
    writeln("  Close braces: ", closes);
    writeln("  Balance:      ", balance);

    if (balance == 0) {
        writeln("  Status: BALANCED");
    } else if (balance > 0) {
        writeln("  Status: UNBALANCED (", balance, " unclosed opening braces)");
    } else {
        writeln("  Status: UNBALANCED (", -balance, " extra closing braces)");
    }
}

void checkWithDepth(string filepath) {
    auto lines = readText(filepath).split("\n");
    int depth = 0;
    int maxDepth = 0;
    int minDepth = 0;
    bool foundNeg = false;

    foreach (i, line; lines) {
        foreach (ch; line) {
            if (ch == '{')
                depth++;
            if (ch == '}')
                depth--;
        }
        if (depth > maxDepth)
            maxDepth = depth;
        if (depth < minDepth)
            minDepth = depth;
        if (depth < 0 && !foundNeg) {
            foundNeg = true;
            writeln("  First negative depth at line ", i + 1, ": depth=", depth, " -> ", line);
        }
    }

    writeln("File: ", filepath);
    writeln("  Total lines: ", lines.length);
    writeln("  Max depth:   ", maxDepth);
    writeln("  Min depth:   ", minDepth);
    writeln("  Final depth: ", depth);

    if (depth == 0 && !foundNeg) {
        writeln("  Status: BALANCED");
    } else {
        writeln("  Status: PROBLEM DETECTED");
    }
}

void main(string[] args) {
    if (args.length < 2) {
        writeln("Usage: check_braces <file.d>");
        writeln("  --simple  : Simple brace count");
        writeln("  --depth   : Track brace depth");
        writeln("  (default) : Run both checks");
        return;
    }

    string filepath = args[1];
    string mode = (args.length > 2) ? args[2] : "both";

    if (!exists(filepath)) {
        writeln("ERROR: File not found: ", filepath);
        return;
    }

    if (mode == "--simple") {
        checkSimple(filepath);
    } else if (mode == "--depth") {
        checkWithDepth(filepath);
    } else {
        checkSimple(filepath);
        writeln();
        checkWithDepth(filepath);
    }
}
