module llm.metric.feedback;

import logger = std.logger;
import std.algorithm : filter, map, sort, uniq, count;
import std.array : array, appender;
import std.conv : to;
import std.format : format;
import std.numeric : cosineSimilarity;
import std.range : take;
import std.string : startsWith;

import llm.metric.monitor : ToolCallEvent;

/// Analyzes recent tool call failures and generates warnings for repeated patterns
struct FeedbackEngine {
    private {
        ToolCallEvent[] events;
        immutable MaxWarnings = 5;
        immutable SimilarityThreshold = 0.5;
        immutable MaxRecentEvents = 1000;
    }

    /// Set events to analyze
    void setEvents(ToolCallEvent[] events) @safe {
        this.events = events.length > MaxRecentEvents
            ? events[events.length - MaxRecentEvents .. $] : events[];
    }

    /// Get warnings about repeated failure patterns
    string[] getWarnings() @safe {
        auto failures = events.filter!(e => !e.success).array;
        return analyzePatterns(failures);
    }

private:

    string[] analyzePatterns(ToolCallEvent[] failures) @safe {
        bool[string] countedTools;
        string[] warnings;

        foreach (failure; failures.filter!(a => a.toolName !in countedTools)) {
            if (warnings.length >= MaxWarnings)
                break;

            // Find similar failures
            auto similarCount = countSimilarFailures(failure, failures);
            if (similarCount >= 2) {
                countedTools[failure.toolName] = true;
                warnings ~= format!"%s: This tool failed %s times with similar arguments. Consider checking the input or tool configuration."(
                        failure.toolName, similarCount);
            }
        }

        return warnings;
    }

    int countSimilarFailures(ToolCallEvent target, ToolCallEvent[] allFailures) @safe {
        int count = 1; // Count the target itself
        foreach (other; allFailures.filter!(a => a.toolName == target.toolName)) {
            if (similarity(other.arguments.toString,
                    target.arguments.toString) > SimilarityThreshold) {
                count++;
            }
        }
        return count;
    }
}

private:

double similarity(string a, string b) @safe {
    import std.uni : byGrapheme, byCodePoint;

    // Get all unique characters from both strings
    auto allChars = (a ~ b).byGrapheme
        .array
        .sort!((a, b) => a.toHash < b.toHash)
        .uniq
        .byCodePoint
        .to!string;
    auto vecA = toVector(a, allChars);
    auto vecB = toVector(b, allChars);
    return cosineSimilarity(vecA, vecB);
}

// Convert string to character frequency vector based on allChars
double[] toVector(string s, string allChars) @safe {
    return allChars.map!(c => cast(double) s.count(c)).array;
}
