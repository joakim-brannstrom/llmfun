/**
 * Integration tests for LlamaEmbedder using a real GGUF model.
 *
 * These tests load a small embedding model from disk and exercise the
 * full LlamaEmbedder pipeline end-to-end. They are not part of the
 * regular unit test suite because they require a real model binary
 * and llama.cpp shared library.
 *
 * Usage:
 *   cd llmfun/local_model
 *   dub run --config=integration_test -- <path-to-model.gguf>
 *
 * License: MPL-2.0
 */
module llm.local.integration_tests;

import std.algorithm.iteration : reduce;
import std.file : exists;
import std.math : approxEqual, sqrt;
import std.path : absolutePath;
import std.range : zip;
import std.stdio : writefln, writeln, stderr;

import llm.common.embedder : Embedder, EmbedResult, EmbedError,
    LocalEmbedConfig, EmbedConfig, createEmbedder;
import llm.llama.model : Model, LlamaParams, contextEmbedding;
import llm.local.llama_embedder : LlamaEmbedder;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

int passed = 0;
int failed = 0;

void check(string name, bool condition) {
    if (condition) {
        writefln("  PASS: %s", name);
        passed++;
    } else {
        writefln("  FAIL: %s", name);
        failed++;
    }
}

void checkResult(string name, EmbedResult result) {
    bool isOk = result.match!((float[] _) => true, (EmbedError _) => false,);
    check(name, isOk);
}

auto extractEmbedding(EmbedResult result) {
    return result.match!((float[] v) => v, (EmbedError e) => {
        writefln("  ERROR extracting embedding: %s", e.errorMsg);
        return (float[]).init; // empty array
    },);
}

auto extractError(EmbedResult result) {
    return result.match!((float[] _) => {
        throw new Exception("Expected error, got embedding");
    }, (EmbedError e) => e.errorMsg,);
}

double cosineSimilarity(float[] a, float[] b) {
    double dot = 0.0, na = 0.0, nb = 0.0;
    foreach (ai, bi; zip(a, b)) {
        dot += ai * bi;
        na += ai * ai;
        nb += bi * bi;
    }
    double mag = sqrt(na) * sqrt(nb);
    return (mag > 0) ? dot / mag : 0.0;
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

void testBasicEmbedding(LlamaEmbedder embedder) {
    writeln("\n1. Basic embedding");

    auto dims = embedder.dimensions();
    check("dimensions() > 0", dims > 0);

    auto result = embedder.embed("hello world");
    checkResult("embed('hello world') succeeded", result);

    auto vec = extractEmbedding(result);
    check("vector length == dimensions()", vec.length == dims);
    check("vector is not all zeros", reduce!((a, b) => a + b)(vec) != 0.0);
}

void testDifferentInputs(LlamaEmbedder embedder) {
    writeln("\n2. Different inputs produce different vectors");

    auto a = extractEmbedding(embedder.embed("hello world"));
    auto b = extractEmbedding(embedder.embed("goodbye moon"));

    auto sim = cosineSimilarity(a, b);
    check("cosine similarity < 1.0", sim < 1.0);
    writefln("  Similarity: %.6f", sim);
}

void testIdenticalInputs(LlamaEmbedder embedder) {
    writeln("\n3. Identical inputs produce identical vectors");

    auto a = extractEmbedding(embedder.embed("hello world"));
    auto b = extractEmbedding(embedder.embed("hello world"));

    auto sim = cosineSimilarity(a, b);
    check("cosine similarity ≈ 1.0", approxEqual(sim, 1.0, 1e-6));
    writefln("  Similarity: %.10f", sim);
}

void testEmptyString(LlamaEmbedder embedder) {
    writeln("\n4. Empty string returns error");
    writeln("  NOTE: This test assumes the model rejects empty input.");
    writeln("  If it fails, your model may accept empty strings \u2014 that's");
    writeln("  model-specific, not a bug in the embedder.");

    auto result = embedder.embed("");
    bool isError = result.match!((float[] _) => false, (EmbedError _) => true,);
    if (isError) {
        check("empty input returns error EmbedResult", true);
        auto msg = extractError(result);
        check("error message is non-empty", msg.length > 0);
        writefln("  Error: \"%s\"", msg);
    } else {
        writeln("  INFO: Model accepted empty string (returned embedding instead of error)");
        writeln("  This is model-specific behavior, not a bug.");
    }
}

void testDestroyLifecycle(LlamaEmbedder embedder) {
    writeln("\n5. Destroy lifecycle");

    // Methods work before destroy
    auto before1 = embedder.modelName();
    check("modelName() before destroy returns non-empty", before1.length > 0);

    auto dims = embedder.dimensions();
    check("dimensions() before destroy > 0", dims > 0);

    embedder.destroy();

    // Methods return safe defaults after destroy
    auto afterName = embedder.modelName();
    check("modelName() after destroy returns '<destroyed>'", afterName == "<destroyed>");

    auto afterDims = embedder.dimensions();
    check("dimensions() after destroy returns 0", afterDims == 0);

    auto afterBatch = embedder.batchSize();
    check("batchSize() after destroy returns 0", afterBatch == 0);

    auto afterEmbed = embedder.embed("test");
    bool isError = afterEmbed.match!((float[] _) => false, (EmbedError _) => true,);
    check("embed() after destroy returns error", isError);

    // destroy is idempotent
    embedder.destroy();
    check("second destroy() does not crash", true);
}

void testMultipleInstances(string modelPath) {
    writeln("\n6. Multiple instances");

    auto params1 = LlamaParams.make();
    params1 = contextEmbedding(params1, 512);
    params1.ctx.n_ctx = 512;

    auto params2 = LlamaParams.make();
    params2 = contextEmbedding(params2, 64);
    params2.ctx.n_ctx = 128;

    auto m1 = new Model(modelPath, params1);
    auto m2 = new Model(modelPath, params2);

    auto e1 = new LlamaEmbedder(m1);
    auto e2 = new LlamaEmbedder(m2);

    check("instance 1 dimensions() > 0", e1.dimensions() > 0);
    check("instance 2 dimensions() > 0", e2.dimensions() > 0);

    auto r1 = e1.embed("test one");
    auto r2 = e2.embed("test two");

    checkResult("instance 1 embed succeeded", r1);
    checkResult("instance 2 embed succeeded", r2);

    e1.destroy();
    e2.destroy();
    check("both instances destroyed without crash", true);
}

void testPluginIntegration(string modelPath) {
    writeln("\n7. Plugin integration (createEmbedder)");

    auto config = LocalEmbedConfig(modelPath, // modelPath
            512, // context
            512, // nBatch
            0, // dimensions — 0 means "auto-detect from model"
            );

    auto embedder = createEmbedder(EmbedConfig(config));
    check("createEmbedder returns non-null", embedder !is null);

    if (embedder !is null) {
        auto name = embedder.modelName();
        check("modelName() is non-empty", name.length > 0);
        writefln("  Model: %s", name);

        auto result = embedder.embed("plugin integration test");
        checkResult("embed via plugin succeeded", result);

        embedder.destroy();
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(string[] args) {
    if (args.length < 2) {
        stderr.writeln("Usage: integration_tests <path-to-model.gguf>");
        return 1;
    }

    auto modelPath = args[1].absolutePath;
    if (!exists(modelPath)) {
        stderr.writefln("Model file not found: %s", modelPath);
        return 1;
    }

    writefln("Integration tests for LlamaEmbedder");
    writefln("Model: %s", modelPath);

    // Load the model once for tests that share it
    auto params = LlamaParams.make();
    params = contextEmbedding(params, 512);
    params.ctx.n_ctx = 512;

    auto model = new Model(modelPath, params);
    auto embedder = new LlamaEmbedder(model);

    // Run shared-model tests
    testBasicEmbedding(embedder);
    testDifferentInputs(embedder);
    testIdenticalInputs(embedder);
    testEmptyString(embedder);

    // WARNING: testDestroyLifecycle DESTROYS the shared embedder.
    // All subsequent shared-model tests will see destroyed state.
    // Do NOT add shared-model tests after this point.
    testDestroyLifecycle(embedder);

    // Test multiple instances (creates its own models)
    testMultipleInstances(modelPath);

    // Test plugin integration (creates its own model via factory)
    testPluginIntegration(modelPath);

    // Summary
    writeln("\n----------------------------------------");
    writefln("Results: %d passed, %d failed", passed, failed);
    writeln("----------------------------------------");

    return (failed > 0) ? 1 : 0;
}
