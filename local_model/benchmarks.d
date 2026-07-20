/**
 * Performance benchmarks for LlamaEmbedder using a real GGUF model.
 *
 * These benchmarks measure latency, memory allocation, and throughput
 * of the embed() method across different input sizes. They require a
 * real model binary and llama.cpp shared library.
 *
 * Usage:
 *   cd llmfun/local_model
 *   dub run --config=benchmark -- <path-to-model.gguf>
 *
 * License: MPL-2.0
 */
module llm.local.benchmarks;

import std.datetime.stopwatch : AutoStart, StopWatch;
import std.file : exists;
import std.path : absolutePath;
import std.stdio : writefln, writeln, stderr;
import std.sumtype : match;

import core.memory : GC;

import llm.common.embedder : Embedder, EmbedError;
import llm.llama.model : Model, LlamaParams, contextEmbedding;
import llm.local.llama_embedder : LlamaEmbedder;

// ---------------------------------------------------------------------------
// Benchmark helpers
// ---------------------------------------------------------------------------

/// Generate a text of approximately `tokenCount` tokens by repeating a known
/// short phrase. Exact token count depends on the model's tokenizer; this
/// yields a rough approximation.
string generateText(size_t approxTokens) {
    // "hello world" is typically 2 tokens in most BPE tokenizers
    // Using a slightly longer phrase for more predictable tokenization
    auto phrase = "The quick brown fox jumps over the lazy dog. ";
    // This phrase is typically ~10-12 tokens
    immutable size_t tokensPerPhrase = 10;

    size_t repeats = (approxTokens + tokensPerPhrase - 1) / tokensPerPhrase;
    string result;
    result.reserve(repeats * phrase.length);
    foreach (_; 0 .. repeats) {
        result ~= phrase;
    }
    return result;
}

/// Run a benchmark function `fn` for `iterations` iterations, recording
/// timing and GC memory stats before and after each call.
struct BenchResult {
    string name;
    long iterations;
    double totalMs;
    double avgMs;
    double opsPerSec;
    size_t memoryDelta; // bytes allocated per iteration (GC usedSize delta)
    size_t totalMemoryDelta;
}

/// Measure the latency of a single operation by running it multiple times
/// and collecting timing + GC stats.
///
/// Note: GC memory delta is approximate. GC collections during the
/// benchmark loop can free allocations from earlier iterations, causing
/// the reported per-call memory delta to be an underestimate.
BenchResult benchmarkLatency(string name, long iterations, void delegate() fn) {
    // Warm-up: run a few iterations to prime caches and trigger any lazy init
    foreach (_; 0 .. 3) {
        fn();
    }

    // Force GC collection before measurement to get a clean baseline
    GC.collect();
    auto baselineUsed = GC.stats().usedSize;

    auto sw = StopWatch(AutoStart.yes);
    foreach (_; 0 .. iterations) {
        fn();
    }
    sw.stop();

    // Measure memory change after all iterations
    GC.collect();
    auto finalUsed = GC.stats().usedSize;

    auto totalMs = sw.peek.total!("msecs");
    auto avgMs = totalMs / iterations;
    auto opsPerSec = (iterations * 1000.0) / totalMs;
    auto totalMemoryDelta = (finalUsed >= baselineUsed) ? (finalUsed - baselineUsed) : 0;
    auto memoryDelta = totalMemoryDelta / iterations;

    return BenchResult(name, iterations, totalMs, avgMs, opsPerSec, memoryDelta, totalMemoryDelta);
}

/// Helper: perform a single embed() call via the interface, extracting the
/// vector to force evaluation of the SumType.
void doEmbed(Embedder emb, string text) {
    auto result = emb.embed(text);
    // Force evaluation: match on the result to ensure no lazy evaluation
    result.match!((float[] v) {
        if (v.length == 0) {
        }
    }, // touch the vector
            (EmbedError e) { /* error — still evaluate the SumType */ },);
}

// ---------------------------------------------------------------------------
// Individual benchmarks
// ---------------------------------------------------------------------------

void benchLatencyShortText(LlamaEmbedder embedder) {
    writeln("\n--- Benchmark 1: Latency — Short Text ---");

    auto text = "hello world";

    auto r = benchmarkLatency("embed('hello world')", 20, {
        doEmbed(embedder, text);
    });

    writefln("  Iterations:       %d", r.iterations);
    writefln("  Total time:       %.2f ms", r.totalMs);
    writefln("  Average latency:  %.3f ms", r.avgMs);
    writefln("  Throughput:       %.1f ops/sec", r.opsPerSec);
    writefln("  Memory/op:        %d bytes", r.memoryDelta);
    writefln("  Status:           %s", (r.avgMs < 100.0)
            ? "PASS (<100ms target)" : "WARNING (exceeds 100ms)");
}

void benchLatencyMediumText(LlamaEmbedder embedder) {
    writeln("\n--- Benchmark 2: Latency — Medium Text (~512 tokens) ---");

    auto text = generateText(512);
    writefln("  Input length: %d chars", text.length);

    auto r = benchmarkLatency("embed(~512 tokens)", 10, {
        doEmbed(embedder, text);
    });

    writefln("  Iterations:       %d", r.iterations);
    writefln("  Total time:       %.2f ms", r.totalMs);
    writefln("  Average latency:  %.3f ms", r.avgMs);
    writefln("  Throughput:       %.1f ops/sec", r.opsPerSec);
    writefln("  Memory/op:        %d bytes", r.memoryDelta);
}

void benchLatencyLongText(LlamaEmbedder embedder) {
    writeln("\n--- Benchmark 3: Latency — Long Text (~2048 tokens) ---");

    auto text = generateText(2048);
    writefln("  Input length: %d chars", text.length);

    auto r = benchmarkLatency("embed(~2048 tokens)", 5, {
        doEmbed(embedder, text);
    });

    writefln("  Iterations:       %d", r.iterations);
    writefln("  Total time:       %.2f ms", r.totalMs);
    writefln("  Average latency:  %.3f ms", r.avgMs);
    writefln("  Throughput:       %.1f ops/sec", r.opsPerSec);
    writefln("  Memory/op:        %d bytes", r.memoryDelta);
}

void benchThroughputBatchBoundary(LlamaEmbedder embedder) {
    writeln("\n--- Benchmark 4: Throughput at Batch Boundaries ---");

    auto batchSize = embedder.batchSize();
    assert(batchSize >= 2, "Batch size must be at least 2 for boundary benchmarks");
    writefln("  Batch capacity: %d tokens", batchSize);

    // Test at ~50% batch capacity
    auto textHalf = generateText(batchSize / 2);
    auto rHalf = benchmarkLatency("embed(~50% batch)", 10, {
        doEmbed(embedder, textHalf);
    });
    writefln("  At 50%% batch (%d tokens approx):", batchSize / 2);
    writefln("    Average: %.3f ms | %.1f ops/sec | %d bytes/op", rHalf.avgMs,
            rHalf.opsPerSec, rHalf.memoryDelta);

    // Test at ~90% batch capacity (near boundary)
    auto textNear = generateText(cast(size_t)(batchSize * 0.9));
    auto rNear = benchmarkLatency("embed(~90% batch)", 5, {
        doEmbed(embedder, textNear);
    });
    writefln("  At 90%% batch (%d tokens approx):", cast(int)(batchSize * 0.9));
    writefln("    Average: %.3f ms | %.1f ops/sec | %d bytes/op", rNear.avgMs,
            rNear.opsPerSec, rNear.memoryDelta);

    // Test with very short text (baseline for comparison)
    auto textShort = "hi";
    auto rShort = benchmarkLatency("embed('hi') baseline", 20, {
        doEmbed(embedder, textShort);
    });
    writefln("  Baseline (2 chars):");
    writefln("    Average: %.3f ms | %.1f ops/sec | %d bytes/op", rShort.avgMs,
            rShort.opsPerSec, rShort.memoryDelta);

    // Calculate throughput scaling factor
    if (rShort.avgMs > 0) {
        writefln("  Scaling factor (50%% batch / baseline): %.2fx", rHalf.avgMs / rShort.avgMs);
        writefln("  Scaling factor (90%% batch / baseline): %.2fx", rNear.avgMs / rShort.avgMs);
    }
}

void benchMemoryAllocation(LlamaEmbedder embedder) {
    // Note: GC memory delta is approximate. GC collections during the
    // benchmark loop can free allocations from earlier iterations, causing
    // the reported per-call memory delta to be an underestimate.
    writeln("\n--- Benchmark 5: Memory Allocation Analysis ---");

    string[] testTexts = [
        "a", // single token
        "hello world", // short (~2 tokens)
        generateText(128), // medium (~128 tokens)
        generateText(512), // large (~512 tokens)
    ];

    foreach (text; testTexts) {
        // First call to trigger any lazy init
        doEmbed(embedder, text);

        // Collect baseline
        GC.collect();
        auto baseline = GC.stats().usedSize;

        // Run several iterations
        immutable reps = 5;
        foreach (_; 0 .. reps) {
            doEmbed(embedder, text);
        }

        GC.collect();
        auto after = GC.stats().usedSize;
        auto delta = (after >= baseline) ? (after - baseline) : 0;

        writefln("  Text length %6d chars: %6d bytes total over %d calls (%d bytes/call)",
                text.length, delta, reps, delta / reps);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(string[] args) {
    if (args.length < 2) {
        stderr.writeln("Usage: benchmarks <path-to-model.gguf>");
        return 1;
    }

    auto modelPath = args[1].absolutePath;
    if (!exists(modelPath)) {
        stderr.writefln("Model file not found: %s", modelPath);
        return 1;
    }

    writefln("Performance Benchmarks for LlamaEmbedder");
    writefln("Model: %s", modelPath);

    // Load the model
    auto params = LlamaParams.make();
    params = contextEmbedding(params, 512);
    params.ctx.n_ctx = 512;

    auto model = new Model(modelPath, params);
    auto embedder = new LlamaEmbedder(model);

    writefln("Model name: %s", embedder.modelName());
    writefln("Dimensions: %d", embedder.dimensions());
    writefln("Batch size: %d", embedder.batchSize());

    // Run benchmarks
    benchLatencyShortText(embedder);
    benchLatencyMediumText(embedder);
    benchLatencyLongText(embedder);
    benchThroughputBatchBoundary(embedder);
    benchMemoryAllocation(embedder);

    writeln("\n----------------------------------------");
    writeln("Benchmarks complete.");
    writeln("----------------------------------------");

    embedder.destroy();
    return 0;
}
