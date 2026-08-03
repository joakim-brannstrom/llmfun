module llm.metric.feedback;

import logger = std.logger;
import std.algorithm : filter, map, sort, uniq, count;
import std.array : array, appender;
import std.conv : to, text;
import std.json : JSONOptions;
import std.numeric : cosineSimilarity;
import std.range : take;
import std.string : startsWith, join;
import std.typecons : Tuple;

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
    string getWarnings() @safe {
        auto failures = events.filter!(e => !e.success).array;
        return analyzePatterns(failures);
    }

private:

    string analyzePatterns(ToolCallEvent[] failures) @safe {
        bool[string] countedTools;
        string[] warnings;

        warnings ~= "[SYSTEM MONITORING NOTE]: Based on historical analysis across current and previous sessions the following tool has frequently failed to be used correctly.";
        foreach (failure; failures.filter!(a => a.toolName !in countedTools)) {
            if (warnings.length >= (MaxWarnings + 1))
                break;

            // Find similar failures
            auto similarCount = countSimilarFailures(failure, failures);

            if (similarCount.count >= 2) {
                countedTools[failure.toolName] = true;
                warnings ~= i"$(failure.toolName): Failed $(similarCount.count) times. Common error: $(
                        similarCount.error)".text;
            }
        }

        return warnings.join("\n");
    }

    Tuple!(int, "count", string, "error", double, "score") countSimilarFailures(
            ToolCallEvent target, ToolCallEvent[] allFailures) @safe {
        import llm.utility : summarizeToolCallArguments;

        typeof(return) rval;
        rval.count = 1; // count the target itself
        rval.score = 0.0;
        foreach (other; allFailures.filter!(a => a.toolName == target.toolName)) {
            const score = similarity(other.arguments.toString(JSONOptions.doNotEscapeSlashes),
                    target.arguments.toString(JSONOptions.doNotEscapeSlashes));
            if (score > SimilarityThreshold) {
                rval.count++;
                if (score > rval.score) {
                    rval.error = other.result.length < 100 ? other.result : other.result[0 .. 100];
                    rval.score = score;
                }
            }
        }
        return rval;
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
