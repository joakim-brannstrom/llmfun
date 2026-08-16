/// YAML loading and JSONValue bridge for llmfun configuration files.
///
/// Human-authored configuration files are written in YAML and parsed with
/// dyaml. The rest of the configuration pipeline works on `std.json.JSONValue`
/// (the reflection merger in `llm.config` and the execution-environment parser
/// in `llm.environment.config`). This module is the single seam between the two
/// worlds: it loads a YAML file into a dyaml `Node` and converts the node into
/// a `JSONValue` that is value-identical to what `std.json.parseJSON` produces
/// for JSON-syntax input (for the JSON-syntax subset covered by the unit
/// tests). Two known YAML 1.1 deviations from full JSON fidelity are
/// intentional and documented: date-like scalars (`2024-01-01`) become
/// timestamps normalized to ISO-ext strings, and exponent-form numbers
/// (`1e5`, `1.5e3`) resolve as strings, whereas `std.json.parseJSON` would
/// yield floating values.
///
/// Machine-managed JSON (state files, chat sessions, protocol payloads) is out
/// of scope and stays with `std.json`.
module llm.yaml;

import logger = std.logger;
import std.datetime : SysTime;
import std.file : readText;
import std.format : format;
import std.json : JSONValue;

import my.path : Path;

import dyaml;

/// Load a YAML document from a file.
///
/// The read and parse phases are wrapped separately so that any failure throws
/// a single `Exception` whose message contains the file path and the failing
/// phase. Empty or comment-only files (dyaml "Zero documents in stream") and
/// multi-document streams ("More than one document in stream") get descriptive
/// messages that include the file path.
///
/// Note: the file path is passed to dyaml as the stream name, so parse-error
/// messages may contain the path twice (once from this wrapper, once from
/// dyaml's own "Unable to load <path>" text and mark info). This is deliberate
/// — dyaml's mark carries the useful line/column position.
///
/// Returns: Root node of the single YAML document.
Node loadYamlNode(Path filePath) {
    string content;
    try {
        content = readText(filePath);
    } catch (Exception e) {
        throw new Exception("Failed to read YAML file %s: %s".format(filePath, e.msg));
    }

    try {
        return Loader.fromString(content, filePath.toString).load();
    } catch (Exception e) {
        throw new Exception("Failed to parse YAML file %s: %s".format(filePath, e.msg));
    }
}

/// Maximum number of nodes converted by a single `yamlToJson` call.
///
/// dyaml loads YAML aliases by shared storage (a "billion laughs" document
/// loads in O(file size)), but conversion to `JSONValue` materializes the
/// expanded tree — visits grow exponentially with alias nesting. This cap
/// bounds both CPU and memory for adversarial input (e.g. an untrusted CWD
/// overlay config). Legitimate configuration files are orders of magnitude
/// below it.
private enum MaxYamlToJsonNodes = 100_000;

/// Convert a dyaml `Node` to a `JSONValue`.
///
/// Each node type maps explicitly (no scalar coercion):
/// null → JSON null, boolean → JSON bool, integer → JSON integer,
/// decimal → JSON float, string → JSON string, timestamp → ISO-ext string,
/// sequence → JSON array, mapping → JSON object, binary → warning + JSON null.
///
/// There is deliberately no `NodeType.merge` branch: dyaml's composer flattens
/// merge (`<<`) keys at load time, so the bridge never sees a merge node.
///
/// Mapping keys are converted with `as!string` (YAML 1.1 scalar conversion).
/// Non-string keys such as `1:` become the string `"1"`; two distinct keys of
/// different types that stringify identically (`1:` and `"1":`) therefore
/// collide and the last one wins. dyaml's duplicate-key throw only covers keys
/// of the same type, so this cross-type collision is a documented behavior
/// difference vs. `std.json`.
JSONValue yamlToJson(Node node) {
    size_t visited;
    return yamlToJsonImpl(node, visited);
}

private JSONValue yamlToJsonImpl(Node node, ref size_t visited) {
    if (++visited > MaxYamlToJsonNodes) {
        throw new Exception(("YAML conversion aborted: more than %d nodes"
                ~ " (possible alias expansion bomb)").format(MaxYamlToJsonNodes));
    }
    JSONValue output;
    switch (node.type) {
        case NodeType.null_:
            output = JSONValue(null);
            break;
        case NodeType.boolean:
            output = JSONValue(node.as!bool);
            break;
        case NodeType.integer:
            output = JSONValue(node.as!long);
            break;
        case NodeType.decimal:
            output = JSONValue(node.as!real);
            break;
        case NodeType.string:
            output = JSONValue(node.as!string);
            break;
        case NodeType.timestamp:
            output = JSONValue(node.as!SysTime.toISOExtString());
            break;
        case NodeType.sequence:
            output = JSONValue(JSONValue[].init);
            foreach (Node child; node)
                output.array ~= yamlToJsonImpl(child, visited);
            break;
        case NodeType.mapping:
            output = JSONValue(string[string].init);
            foreach (Node keyNode, Node valueNode; node)
                output[keyNode.as!string] = yamlToJsonImpl(valueNode, visited);
            break;
        case NodeType.binary:
            logger.warningf("YAML binary value ignored (converted to null)");
            output = JSONValue(null);
            break;
        default:
            // NodeType.invalid (and NodeType.merge, which the composer resolves
            // at load time — this is dead defensive code).
            logger.warningf("Unexpected YAML node type %s ignored (converted to null)", node.type);
            output = JSONValue(null);
            break;
    }
    return output;
}

/// Convenience: load a YAML file and convert it to a `JSONValue`.
///
/// Every llmfun configuration file is a YAML mapping, so a non-mapping root is
/// rejected with a clear message naming the file. Conversion failures (e.g. an
/// out-of-range numeric scalar) are wrapped with the file path so the error
/// contract of `loadYamlNode` also holds for the convenience function.
JSONValue loadYamlValue(Path filePath) {
    auto node = loadYamlNode(filePath);
    if (node.type != NodeType.mapping) {
        throw new Exception(
                "YAML config %s: root must be a mapping, got %s".format(filePath, node.type));
    }
    try {
        return yamlToJson(node);
    } catch (Exception e) {
        throw new Exception("Failed to convert YAML file %s: %s".format(filePath, e.msg));
    }
}

/// Test: JSON-syntax YAML input round-trips to the same JSONValue as std.json.parseJSON.
unittest {
    import std.conv : to;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.json : parseJSON;
    import std.path : buildPath;
    import std.stdio : File;

    immutable content = `{"name": "test", "count": 42, "ratio": 0.5, "enabled": true, "tags": ["a", "b"], "nested": {"x": 1}, "nothing": null}`;
    auto tmpDir = buildPath("llmfun_test", "yaml_roundtrip_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "test.yaml");
    File(tmpFile, "w").write(content);

    auto got = loadYamlValue(Path(tmpFile));
    auto expected = parseJSON(content);
    assert(got == expected, "YAML round trip differs from std.json.parseJSON");
}

/// Test: every node type maps to the expected JSONValue.
unittest {
    import std.json : JSONType, parseJSON;

    immutable content = `
nullVal: null
yesBool: yes
quotedYes: "yes"
intVal: 42
decVal: 0.5
strVal: hello
seqVal: [1, 2, 3]
mapVal:
  inner: value
tsVal: 2024-01-01
binVal: !!binary aGVsbG8=
`;
    auto json = yamlToJson(Loader.fromString(content).load());
    assert(json["nullVal"].type == JSONType.null_);
    assert(json["yesBool"].get!bool == true);
    assert(json["quotedYes"].str == "yes");
    assert(json["intVal"].integer == 42);
    assert(json["decVal"].floating == 0.5);
    assert(json["strVal"].str == "hello");
    assert(json["seqVal"].array.length == 3);
    assert(json["seqVal"][1].integer == 2);
    assert(json["mapVal"]["inner"].str == "value");
    assert(json["tsVal"].str == "2024-01-01T00:00:00Z");
    assert(json["binVal"].type == JSONType.null_, "binary node must convert to null");

    // Sanity: parseJSON of a JSON-syntax null/true also yields null/true.
    auto probe = parseJSON(`{"a": null, "b": true}`);
    assert(probe["a"].type == JSONType.null_);
    assert(probe["b"].type == JSONType.true_);
}

/// Test: merge keys (`<<`) are flattened by the composer at load time.
unittest {
    immutable content = `
base: &b
  a: 1
  b: 2
derived:
  <<: *b
  z: 3
`;
    auto json = yamlToJson(Loader.fromString(content).load());
    assert(json["derived"]["a"].integer == 1);
    assert(json["derived"]["b"].integer == 2);
    assert(json["derived"]["z"].integer == 3);
}

/// Test: multi-document stream is rejected with a descriptive message including the path.
unittest {
    import std.conv : to;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.algorithm.searching : canFind;

    auto tmpDir = buildPath("llmfun_test", "yaml_multidoc_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "multi.yaml");
    File(tmpFile, "w").write("--- 4\n--- 6\n");

    bool caught = false;
    try {
        loadYamlValue(Path(tmpFile));
    } catch (Exception e) {
        caught = true;
        assert(e.msg.canFind("More than one document in stream"), e.msg);
        assert(e.msg.canFind(tmpFile), e.msg);
    }
    assert(caught, "multi-document stream must throw");
}

/// Test: empty file is rejected with a descriptive message including the path.
unittest {
    import std.conv : to;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.algorithm.searching : canFind;

    auto tmpDir = buildPath("llmfun_test", "yaml_empty_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "empty.yaml");
    File(tmpFile, "w").write("");

    bool caught = false;
    try {
        loadYamlValue(Path(tmpFile));
    } catch (Exception e) {
        caught = true;
        assert(e.msg.canFind("Zero documents in stream"), e.msg);
        assert(e.msg.canFind(tmpFile), e.msg);
    }
    assert(caught, "empty file must throw");
}

/// Test: comment-only file is rejected like an empty file.
unittest {
    import std.conv : to;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.algorithm.searching : canFind;

    auto tmpDir = buildPath("llmfun_test", "yaml_comment_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "comment.yaml");
    File(tmpFile, "w").write("# just a comment\n");

    bool caught = false;
    try {
        loadYamlValue(Path(tmpFile));
    } catch (Exception e) {
        caught = true;
        assert(e.msg.canFind("Zero documents in stream"), e.msg);
        assert(e.msg.canFind(tmpFile), e.msg);
    }
    assert(caught, "comment-only file must throw");
}

/// Test: read failure (nonexistent file) mentions the path and the read phase.
unittest {
    import std.algorithm.searching : canFind;

    auto missing = Path("llmfun_test/yaml_nonexistent_read_test.yaml");
    bool caught = false;
    try {
        loadYamlNode(missing);
    } catch (Exception e) {
        caught = true;
        assert(e.msg.canFind("Failed to read YAML file"), e.msg);
        assert(e.msg.canFind(missing.toString), e.msg);
    }
    assert(caught, "nonexistent file must throw");
}

/// Test: parse failure (`@`-leading plain scalar) mentions the path and the parse phase.
unittest {
    import std.conv : to;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.algorithm.searching : canFind;

    auto tmpDir = buildPath("llmfun_test", "yaml_parseerr_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "bad.yaml");
    File(tmpFile, "w").write("value: @{llmfun_workarea}\n");

    bool caught = false;
    try {
        loadYamlValue(Path(tmpFile));
    } catch (Exception e) {
        caught = true;
        assert(e.msg.canFind("Failed to parse YAML file"), e.msg);
        assert(e.msg.canFind(tmpFile), e.msg);
        assert(e.msg.canFind("cannot start any token"), e.msg);
    }
    assert(caught, "parse error must throw");
}

/// Test: non-mapping root is rejected with the file path.
unittest {
    import std.conv : to;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.algorithm.searching : canFind;

    auto tmpDir = buildPath("llmfun_test", "yaml_roottype_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "scalar_root.yaml");
    File(tmpFile, "w").write("just a scalar\n");

    bool caught = false;
    try {
        loadYamlValue(Path(tmpFile));
    } catch (Exception e) {
        caught = true;
        assert(e.msg.canFind("root must be a mapping"), e.msg);
        assert(e.msg.canFind(tmpFile), e.msg);
    }
    assert(caught, "scalar root must throw");
}

/// Test: conversion failure is wrapped with the file path.
unittest {
    import std.conv : to;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.algorithm.searching : canFind;

    auto tmpDir = buildPath("llmfun_test", "yaml_convert_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "complex_key.yaml");
    // A complex mapping key (a mapping used as a key) loads fine, but dyaml
    // cannot convert it to a string — `as!string` on the key throws, which
    // loadYamlValue must wrap with the file path.
    File(tmpFile, "w").write("? {x: 1}\n: value\n");

    bool caught = false;
    try {
        loadYamlValue(Path(tmpFile));
    } catch (Exception e) {
        caught = true;
        assert(e.msg.canFind("Failed to convert YAML file"), e.msg);
        assert(e.msg.canFind(tmpFile), e.msg);
    }
    assert(caught, "conversion failure must throw with path");
}

/// Test: alias-expansion bomb is bounded by the conversion node cap.
unittest {
    import std.conv : to;
    import std.algorithm.searching : canFind;

    // Each level references the previous level 9 times. dyaml loads this with
    // shared storage (cheap), but conversion would materialize 9^levels nodes.
    string bomb = "l0: &l0 [x]\n";
    foreach (i; 1 .. 7) {
        bomb ~= "l" ~ i.to!string ~ ": &l" ~ i.to!string ~ " [";
        foreach (j; 0 .. 9) {
            if (j > 0)
                bomb ~= ", ";
            bomb ~= "*l" ~ (i - 1).to!string;
        }
        bomb ~= "]\n";
    }

    bool caught = false;
    try {
        yamlToJson(Loader.fromString(bomb).load());
    } catch (Exception e) {
        caught = true;
        assert(e.msg.canFind("possible alias expansion bomb"), e.msg);
    }
    assert(caught, "alias expansion bomb must be rejected by the node cap");
}
