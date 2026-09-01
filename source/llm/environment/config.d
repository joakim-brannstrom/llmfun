/// Configuration types for environment-based command execution.
/// Defines ExecutionType, CommandJoinMode, ContainerConfig, HostConfig,
/// and EnvironmentBackend with SumType-based config separation.
module llm.environment.config;

import logger = std.logger;
import std.algorithm : filter, map;
import std.array : empty, array;
import std.conv : to;
import std.datetime : Duration, dur;
import std.file : exists;
import std.json : JSONValue, JSONType;
import std.string : indexOf;
import std.sumtype : SumType;

import my.path : Path;

import llm.utility : getValue;
import llm.yaml : loadYamlValue;

/// Execution environment type distinguishing container vs host backends.
/// Used by consumers to determine which RunnerBackend to instantiate.
enum ExecutionType {
    /// Execute inside a container (Docker, Podman, etc.)
    container,
    /// Execute directly on the host system via subprocess
    host
}

/// Strategy for combining the LLM's command array into the final command line.
enum CommandJoinMode {
    /// The argument can only be [0, 1] so no joining
    single,
    /// Join all elements with spaces into a single argument string.
    whitespace,
    /// Each element of the array is treated as a separate argument.
    append,
}

/// Configuration for container-based execution environments.
struct ContainerConfig {
    /// Container runtime CLI command (e.g., "docker" or "podman").
    string runtimeCli;

    /// Container image name (e.g., "alpine:latest").
    string image;

    /// Default container options as a map from logical group names to CLI arguments.
    /// Keys are logical groups (e.g., "security", "network", "mounts").
    /// Values are arrays of CLI arguments flattened into the command line.
    string[][string] options;

    invariant {
        assert(runtimeCli.length > 0, "ContainerConfig runtimeCli must not be empty");
        assert(image.length > 0, "ContainerConfig image must not be empty");
    }
}

/// Configuration for host-based execution environments.
struct HostConfig {
    /// Host subprocess options as a map from logical group names to CLI arguments.
    /// Keys are logical groups; values are arrays of CLI arguments.
    string[][string] options;

    /// Working directory for the subprocess (supports magic words).
    /// An empty string defaults to the current directory.
    string workingDir;

    /// Environment variables for the subprocess (supports magic and magic word substitution in values).
    string[string] envVars;

    /// Restrict commands to these specific prefixes.
    string[] allowedCommandPrefixes;
}

/// Union type for environment configuration — either container or host.
alias EnvironmentConfig = SumType!(ContainerConfig, HostConfig);

/// Complete description of an execution environment backend.
struct EnvironmentBackend {
    /// Unique identifier for this environment (used in tool calls).
    string tag;

    /// Human-readable description of what this environment is for.
    string description;

    /// Capability tags for categorization and filtering.
    /// Empty array is valid (no special capabilities).
    string[] capabilities;

    /// True if this environment provides isolation (e.g., container).
    bool isIsolated;

    /// Maximum execution time in seconds.
    Duration timeout;

    /// Environment-specific configuration (container or host).
    EnvironmentConfig config;

    /// How command arguments are combined
    CommandJoinMode commandJoinMode = CommandJoinMode.single;

    invariant {
        assert(tag.length > 0, "EnvironmentBackend tag must not be empty");
        assert(timeout.total!"seconds" >= 0, "EnvironmentBackend timeout must not be negative");
    }
}

/// State tracking for execution environment config loading.
enum ExecutionConfigState {
    /// Config file not found — execution is disabled.
    notConfigured,
    /// Config loaded successfully.
    loaded,
    /// Config failed to load (parse error, invalid format, duplicates, etc.).
    loadFailed
}

/// Parse an options/mounts JSON object into string[][string].
/// Validates keys have the NN_ prefix format.
private string[][string] parseEnvOptions(JSONValue optJson, string tagName, string configKind) {
    import std.string : startsWith;

    string[][string] result;
    if (optJson.type != JSONType.OBJECT) {
        logger.warningf("Invalid options format for %s '%s' - expected object",
                configKind, tagName);
        return result;
    }

    foreach (key, val; optJson.object) {
        if (val.type != JSONType.ARRAY) {
            logger.warningf("Skipping non-array value for options['%s'] in %s '%s'",
                    key, configKind, tagName);
            continue;
        }
        string[] arr;
        foreach (item; val.array) {
            if (item.type == JSONType.STRING) {
                arr ~= item.str;
            } else {
                logger.warningf("Skipping non-string item in options['%s'] for %s '%s'",
                        key, configKind, tagName);
            }
        }
        if (!arr.empty) {
            result[key] = arr;
        }
    }

    string[] invalidKeys;
    foreach (key; result.byKey) {
        if (key.startsWith("entrypoint_")) {
        } else if (key.length < 3 || key[2] != '_') {
            invalidKeys ~= key;
        } else {
            try {
                key[0 .. 2].to!int;
            } catch (Exception e) {
                invalidKeys ~= key;
            }
        }
    }
    if (!invalidKeys.empty) {
        logger.warningf("Configuration for %s '%s' has invalid option key(s) removed. "
                ~ "Keys must be prefixed with <NN>_: %s", configKind, tagName, invalidKeys);
        foreach (key; invalidKeys) {
            result.remove(key);
        }
    }

    return result;
}

private string[][string] merge(string[][string] base, string[][string] add) {
    string[][string] merged;

    foreach (a; base.byKeyValue) {
        merged[a.key] = a.value;
    }
    foreach (a; add.byKeyValue) {
        merged[a.key] = a.value;
    }

    return merged;
}

/// Load execution backends from a YAML configuration file.
///
/// Supports the format:
/// `version: 1 / defaultEnvironment: tag / environments: [...]`
///
/// Accepts `envVars` in both map (`{ "KEY": "VALUE" }`) and list
/// (`["KEY=VALUE"]`) formats, normalizing to `string[string]`.
///
/// Tags must be globally unique — duplicates cause a load failure.
/// Returns an empty array if no config file is found (execution disabled).
///
/// Params:
///    filePath = Path to the YAML config file.
///    state = Output parameter tracking the load outcome.
///    defaultTag = Output parameter for the default environment tag (optional).
///
/// Returns:
///    Array of parsed backends, or empty/null on failure.
EnvironmentBackend[] loadExecutionBackends(Path filePath,
        string[][string] defaultOptions, ref ExecutionConfigState state, ref string defaultTag) {

    state = ExecutionConfigState.loadFailed;
    defaultTag = null;

    if (!filePath.exists) {
        logger.tracef("Execution environments config not found: %s - command execution is disabled",
                filePath);
        state = ExecutionConfigState.notConfigured;
        return null;
    }

    JSONValue json;
    try {
        json = loadYamlValue(filePath);
    } catch (Exception e) {
        logger.warningf("Failed to load execution environments config %s: %s", filePath, e.msg);
        return null;
    }

    if (json.type != JSONType.OBJECT || !("environments" in json)) {
        logger.warningf("Execution environments config %s has invalid format - "
                ~ "expected object with 'environments' field", filePath);
        return null;
    }

    auto envJson = json["environments"];
    if (envJson.type != JSONType.ARRAY) {
        logger.warningf("Execution environments config %s: 'environments' is not an array",
                filePath);
        return null;
    }

    if ("version" in json) {
        // getValue swallows a wrong-typed 'version' (YAML makes this easy:
        // 1.0 → decimal, "1" → string); a non-integer version then degrades
        // to the unknown-version warning instead of failing the whole load.
        auto ver = getValue(json, (v) => v["version"].integer, 0);
        if (ver != 1) {
            logger.warningf("Execution environments config version %s is unknown - " ~ "attempting to parse anyway",
                    ver);
        }
    }
    // TODO: remove this. It is not used
    defaultTag = getValue(json, (v) => v["defaultEnvironment"].str, null);

    EnvironmentBackend[] result;
    bool[string] seenTags;

    foreach (entry; envJson.array) {
        if (entry.type != JSONType.OBJECT) {
            logger.warningf("Skipping non-object entry in execution environments config");
            continue;
        }

        string tag = getValue(entry, (v) => v["tag"].str, null);
        if (tag.empty) {
            logger.warningf("Skipping environment entry with empty or missing 'tag' field");
            continue;
        }

        if (tag in seenTags) {
            logger.warningf("Duplicate environment tag '%s' in config - keeping first occurrence",
                    tag);
            continue;
        }
        seenTags[tag] = true;

        string description = getValue(entry, (v) => v["description"].str, null);

        string[] capabilities = getValue(entry, (v) => v["capabilities"].array, null).filter!(
                a => a.type == JSONType.STRING)
            .map!(a => a.str)
            .array;

        bool isIsolated = getValue(entry, (v) => v["isIsolated"].boolean, false);

        CommandJoinMode commandJoinMode;
        try {
            auto s = getValue(entry, (v) => v["commandJoinMode"].str, null);
            if (s.empty) {
                logger.tracef("Empty commandJoinMode for environment '%s' - defaulting to '%s'",
                        tag, commandJoinMode);
            } else {
                commandJoinMode = getValue(entry, (v) => v["commandJoinMode"].str, null)
                    .to!CommandJoinMode;
            }
        } catch (Exception e) {
            logger.warningf("Unknown commandJoinMode '%s' for environment '%s' - defaulting to '%s'",
                    getValue(entry, (v) => v["commandJoinMode"].str, null), tag, commandJoinMode);
        }

        if ("config" !in entry) {
            logger.warningf("Environment entry '%s' has no 'config' field - skipping", tag);
            continue;
        }

        auto timeout = getValue(entry, (v) => v["timeout"].integer.dur!"seconds", Duration.zero);
        if (timeout == Duration.zero) {
            logger.warningf("Environment entry '%s' has no 'timeout' field - skipping", tag);
            continue;
        }

        EnvironmentConfig config;
        {
            auto configJson = entry["config"];
            if (configJson.type != JSONType.OBJECT) {
                logger.warningf("Invalid config format for environment '%s' - expected object",
                        tag);
                continue;
            }

            string configType = getValue(configJson, (v) => v["type"].str, null);

            if (configType == "container") {
                string runtimeCli = getValue(configJson, (v) => v["runtimeCli"].str, null);
                if (runtimeCli.empty) {
                    logger.warningf("Missing runtimeCli for container environment '%s' - skipping",
                            tag);
                    continue;
                }

                string image = getValue(configJson, (v) => v["image"].str, null);
                if (image.empty) {
                    logger.warningf("Missing image for container environment '%s' - skipping", tag);
                    continue;
                }

                string[][string] options;
                if ("options" in configJson) {
                    options = merge(defaultOptions,
                            parseEnvOptions(configJson["options"], tag, "container"));
                }

                config = EnvironmentConfig(ContainerConfig(runtimeCli, image, options));
            } else if (configType == "host") {
                string[][string] options;
                if ("options" in configJson) {
                    options = parseEnvOptions(configJson["options"], tag, "host");
                }

                string workingDir = getValue(configJson, (v) => v["workingDir"].str, null);

                string[string] envVars;
                if ("envVars" in configJson) {
                    auto envVarsJson = configJson["envVars"];
                    if (envVarsJson.type == JSONType.OBJECT) {
                        foreach (key, val; envVarsJson.object) {
                            if (val.type == JSONType.STRING) {
                                envVars[key] = val.str;
                            } else {
                                logger.warningf(
                                        "Skipping non-string envVar value for key '%s' in host environment '%s'",
                                        key, tag);
                            }
                        }
                    } else if (envVarsJson.type == JSONType.ARRAY) {
                        foreach (item; envVarsJson.array) {
                            if (item.type == JSONType.STRING) {
                                auto str = item.str;
                                auto eqPos = str.indexOf('=');
                                if (eqPos >= 0 && eqPos < str.length) {
                                    if (eqPos == 0) {
                                        logger.warningf(
                                                "Invalid envVar format '%s' in host environment '%s' - empty key",
                                                str, tag);
                                    } else {
                                        envVars[str[0 .. eqPos]] = str[eqPos + 1 .. $];
                                    }
                                }
                            }
                        }
                    } else {
                        logger.warningf("Invalid envVars format in host environment '%s' - expected object or array",
                                tag);
                    }
                }

                string[] allowedCommandPrefixes;
                if ("allowedCommandPrefixes" in configJson) {
                    allowedCommandPrefixes = getValue(configJson,
                            (v) => v["allowedCommandPrefixes"].array, null).filter!(
                            a => a.type == JSONType.STRING)
                        .map!(a => a.str)
                        .array;
                }

                config = EnvironmentConfig(HostConfig(options, workingDir,
                        envVars, allowedCommandPrefixes));
            } else {
                logger.warningf("Unknown configuration in file '%s': %s", filePath, entry);
                continue;
            }
        }

        result ~= EnvironmentBackend(tag, description, capabilities,
                isIsolated, timeout: timeout, config, commandJoinMode);
    }

    if (result.empty) {
        logger.warningf("Execution environments config %s has no valid environments", filePath);
        return null;
    }

    // Validate defaultEnvironment references an existing tag. A typo'd default
    // must not fail the whole config; drop it so executeCommand can report the
    // clear "no default configured" error instead of a confusing tag lookup.
    if (!defaultTag.empty && defaultTag !in seenTags) {
        logger.warningf("defaultEnvironment '%s' in %s does not match any environment tag - " ~ "ignoring default",
                defaultTag, filePath);
        defaultTag = null;
    }

    if (result.length > 100) {
        logger.warningf("Execution environments config contains %s entries (exceeds 100) - "
                ~ "this may impact performance", result.length);
    }

    state = ExecutionConfigState.loaded;
    logger.infof("Loaded %s execution environments from %s", result.length, filePath);

    return result;
}

// Unit tests for loadExecutionBackends()
// ─────────────────────────────────────────────────────────────────────────────

/// Test: valid YAML with container and host environments parses correctly.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `version: 1
defaultEnvironment: sandbox
environments:
  - tag: sandbox
    description: Container sandbox
    capabilities:
      - isolated
    isIsolated: true
    commandJoinMode: whitespace
    timeout: 60
    config:
      type: container
      runtimeCli: docker
      image: alpine:latest
      options:
        00_subcommand: ["run"]
        01_security: ["--read-only"]
        05_workarea: ["-v", "/workarea:/workarea"]
  - tag: native
    description: Host execution
    capabilities:
      - fast
    isIsolated: false
    commandJoinMode: append
    timeout: 30
    config:
      type: host
      options:
        00_shell: ["sh", "-c"]
      workingDir: /tmp
      envVars:
        HOME: /tmp
        PATH: /usr/bin
  - tag: default_value
    description: Default values
    timeout: 30
    config:
      type: container
      runtimeCli: docker
      image: foobar
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 3);
    assert(defaultTag == "sandbox");

    // Verify container environment
    assert(result[0].tag == "sandbox");
    assert(result[0].description == "Container sandbox");
    assert(result[0].isIsolated);
    assert(result[0].commandJoinMode == CommandJoinMode.whitespace);
    assert(result[0].timeout == 60.dur!"seconds");
    assert(result[0].capabilities.length == 1);
    assert(result[0].capabilities[0] == "isolated");
    assert(result[0].config.match!((ContainerConfig c) => c.runtimeCli == "docker"
            && c.image == "alpine:latest" && c.options.length == 3, (HostConfig h) => false));

    // Verify host environment
    assert(result[1].tag == "native");
    assert(result[1].description == "Host execution");
    assert(!result[1].isIsolated);
    assert(result[1].commandJoinMode == CommandJoinMode.append);
    assert(result[1].timeout == 30.dur!"seconds");
    assert(result[1].config.match!((ContainerConfig c) => false,
            (HostConfig h) => h.workingDir == "/tmp"
            && h.envVars["HOME"] == "/tmp" && h.envVars["PATH"] == "/usr/bin"));

    // Verify default values environment
    assert(result[2].tag == "default_value");
    assert(!result[2].isIsolated);
    assert(result[2].commandJoinMode == CommandJoinMode.single);
    assert(result[2].timeout == 30.dur!"seconds");
    assert(result[2].config.match!((ContainerConfig c) => true, (HostConfig h) => false));
}

/// Test: missing file returns null with notConfigured state.
unittest {
    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends("/nonexistent/exec_env.yaml".Path, null, state, defaultTag);
    assert(result is null);
    assert(state == ExecutionConfigState.notConfigured);
    assert(defaultTag is null);
}

/// Test: defaultEnvironment referencing an unknown tag is dropped with a warning.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `version: 1
defaultEnvironment: sandbos
environments:
  - tag: sandbox
    timeout: 60
    config:
      type: container
      runtimeCli: docker
      image: alpine:latest
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    assert(defaultTag is null);
}

/// Test: duplicate tags warn and skip, keeping first occurrence.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: sandbox
    timeout: 60
    config:
      type: container
      runtimeCli: docker
      image: alpine:latest
  - tag: sandbox
    timeout: 30
    config:
      type: host
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    assert(result[0].tag == "sandbox");
}

/// Test: invalid YAML returns null with loadFailed state.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "bad.yaml");
    // Leading '@' is a genuine dyaml parse error ("cannot start any token").
    // '{ invalid json }' would NOT work here: it is valid YAML (a mapping
    // {invalid json: null}), which would move this test off the parse-failure
    // path onto the invalid-format path (same state, wrong branch exercised).
    File(tmpFile, "w").write("@invalid");

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(result is null);
    assert(state == ExecutionConfigState.loadFailed);
}

/// Test: envVars in list format ["KEY=VALUE"] normalizes to map.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: testhost
    timeout: 30
    config:
      type: host
      envVars: ["FOO=bar", "BAZ=qux"]
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    assert(result[0].config.match!((ContainerConfig c) => false,
            (HostConfig h) => h.envVars["FOO"] == "bar" && h.envVars["BAZ"] == "qux"));
}

/// Test: envVars in map format {"KEY": "VALUE"} parses correctly.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: testhost
    timeout: 30
    config:
      type: host
      envVars:
        KEY1: val1
        KEY2: val2
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    assert(result[0].config.match!((ContainerConfig c) => false,
            (HostConfig h) => h.envVars["KEY1"] == "val1" && h.envVars["KEY2"] == "val2"));
}

/// Test: empty environments array returns null (load failed).
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `version: 1
environments: []
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(result is null);
    assert(state == ExecutionConfigState.loadFailed);
}

/// Test: missing "environments" field returns null.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `version: 1
items: []
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(result is null);
    assert(state == ExecutionConfigState.loadFailed);
}

/// Test: container detection by runtimeCli field (no type field).
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: podman-env
    timeout: 60
    config:
      type: container
      runtimeCli: podman
      image: alpine:latest
      options:
        00_run: ["run"]
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    assert(result[0].config.match!((ContainerConfig c) => c.runtimeCli == "podman",
            (HostConfig h) => false));
}

/// Test: default environment tag is returned via output parameter.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `defaultEnvironment: mydefault
environments:
  - tag: mydefault
    timeout: 30
    config:
      type: host
  - tag: other
    timeout: 30
    config:
      type: host
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(defaultTag == "mydefault");
}

/// Test: option key validation — invalid keys are removed.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: test
    timeout: 60
    config:
      type: container
      runtimeCli: docker
      image: alpine:latest
      options:
        00_valid: ["arg1"]
        invalid: ["arg2"]
        01_also_valid: ["arg3"]
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    assert(result[0].config.match!((ContainerConfig c) => c.options.length == 2
            && ("00_valid" in c.options) && ("01_also_valid" in c.options), (HostConfig h) => false));
}

/// Test: envVars with empty value ("KEY=") is accepted.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: testhost
    timeout: 30
    config:
      type: host
      envVars: ["FOO=", "BAR=baz"]
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    assert(result[0].config.match!((ContainerConfig c) => false,
            (HostConfig h) => h.envVars["FOO"] == "" && h.envVars["BAR"] == "baz"));
}

/// Test: envVars with empty key ("=value") is rejected.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: testhost
    timeout: 30
    config:
      type: host
      envVars: ["=bad", "GOOD=ok"]
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    // "=bad" should be rejected, "GOOD=ok" should be accepted
    assert(result[0].config.match!((ContainerConfig c) => false,
            (HostConfig h) => !("" in h.envVars) && h.envVars["GOOD"] == "ok"));
}

/// Test: defaultOptions are merged with container options, environment options override defaults.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    // Environment has only "01_security" option, defaultOptions has "00_subcommand" and "01_security"
    // Result should have both, with environment's "01_security" overriding the default
    string yaml = `environments:
  - tag: test
    timeout: 60
    config:
      type: container
      runtimeCli: docker
      image: alpine:latest
      options:
        01_security: ["--read-only"]
`;
    File(tmpFile, "w").write(yaml);

    // Default options with "00_subcommand" and "01_security"
    string[][string] defaultOptions;
    defaultOptions["00_subcommand"] = ["run", "--rm"];
    defaultOptions["01_security"] = ["--default-security"];

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, defaultOptions, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);

    // Verify merge: "00_subcommand" from defaults is present, "01_security" is overridden by env
    assert(result[0].config.match!((ContainerConfig c) {
            assert("00_subcommand" in c.options, "default option should be merged");
            assert(c.options["00_subcommand"][0] == "run");
            assert(c.options["00_subcommand"][1] == "--rm");
            assert("01_security" in c.options, "env option should override default");
            assert(c.options["01_security"][0] == "--read-only",
            "env option should override default");
            return true;
        }, (HostConfig h) => false));
}

/// Test: defaultOptions with null produces no merge (environment options only).
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: test
    timeout: 60
    config:
      type: container
      runtimeCli: docker
      image: alpine:latest
      options:
        00_run: ["run"]
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);

    // Only environment options should be present
    assert(result[0].config.match!((ContainerConfig c) {
            assert(c.options.length == 1);
            assert("00_run" in c.options);
            return true;
        }, (HostConfig h) => false));
}

/// Test: environment options override defaults.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;
    import std.sumtype : match;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `environments:
  - tag: testhost
    timeout: 30
    config:
      type: host
      options:
        01_env: ["--env", "VAR=val"]
`;
    // Environment has only "01_env" option, defaultOptions has "00_shell" and "01_env"
    // Result should have both, with environment's "01_env" overriding the default
    File(tmpFile, "w").write(yaml);

    // Default options with "00_shell" and "01_env"
    string[][string] defaultOptions;
    defaultOptions["00_shell"] = ["sh", "-c"];
    defaultOptions["01_env"] = ["--default-env"];

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, defaultOptions, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);

    // Verify "01_env" is overridden by env
    assert(result[0].config.match!((ContainerConfig c) => false, (HostConfig h) {
            assert("01_env" in h.options, "env option should override default");
            assert(h.options["01_env"][0] == "--env", "env option should override default");
            assert(h.options["01_env"][1] == "VAR=val");
            return true;
        }));
}

/// Test: a wrong-typed 'version' (YAML decimal `1.0`) degrades to the
/// unknown-version warning instead of throwing and failing the whole load.
unittest {
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.stdio : File;

    auto tmpDir = buildPath("llmfun_test", "execenv_" ~ __LINE__.to!string);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto tmpFile = buildPath(tmpDir, "config.yaml");
    string yaml = `version: 1.0
environments:
  - tag: sandbox
    timeout: 60
    config:
      type: container
      runtimeCli: docker
      image: alpine:latest
`;
    File(tmpFile, "w").write(yaml);

    ExecutionConfigState state;
    string defaultTag;
    auto result = loadExecutionBackends(tmpFile.Path, null, state, defaultTag);
    assert(state == ExecutionConfigState.loaded);
    assert(result.length == 1);
    assert(result[0].tag == "sandbox");
}

/// Test: the shipped config/execution_environments.yaml loads all six
/// environments; container environments keep the workarea mount and network
/// isolation, and the native environment keeps its magic-word workingDir.
unittest {
    import std.sumtype : match;

    // Parse guard: the shipped file must load via the bridge directly.
    auto envJson = loadYamlValue(Path("config/execution_environments.yaml"));
    assert(envJson["version"].integer == 1);
    assert(envJson["environments"].array.length == 6);

    // Use the shipped example.yaml's defaultOptions so the merge path is
    // exercised exactly like the real pipeline (config.d loadExecutionEnvironments).
    string[][string] defaultOptions;
    auto exampleJson = loadYamlValue(Path("config/example.yaml"));
    foreach (key, val; exampleJson["sandboxConfig"]["defaultOptions"].object) {
        string[] vals;
        foreach (v; val.array)
            vals ~= v.str;
        defaultOptions[key] = vals;
    }

    ExecutionConfigState state;
    string defaultTag;
    auto envs = loadExecutionBackends(Path("config/execution_environments.yaml"),
            defaultOptions, state, defaultTag);
    assert(state == ExecutionConfigState.loaded, "shipped environments file must load");
    assert(envs.length == 6, "expected 6 environments, got " ~ envs.length.to!string);

    // Tags in file order.
    assert(envs[0].tag == "llmfun");
    assert(envs[1].tag == "alpine");
    assert(envs[2].tag == "python");
    assert(envs[3].tag == "node");
    assert(envs[4].tag == "ubuntu");
    assert(envs[5].tag == "native");

    // Every container environment: the quoted @{llmfun_workarea} mount and the
    // merged network-isolation default must survive the load.
    foreach (env; envs[0 .. 5]) {
        assert(env.config.match!((ContainerConfig c) {
                assert(c.options["05_mounts"] == [
                    "-v", "@{llmfun_workarea}:/workarea"
                ], "container " ~ env.tag ~ " lost its workarea mount");
                assert(c.options["06_network"] == ["--network", "none"],
                "container " ~ env.tag ~ " lost network isolation");
                return true;
            }, (HostConfig h) => false), "environment " ~ env.tag ~ " must be a container");
    }

    // Native (host) environment: magic-word workingDir, env vars, shell options.
    assert(envs[5].config.match!((ContainerConfig c) => false, (HostConfig h) {
            assert(h.workingDir == "@{llmfun_workarea}");
            assert(h.envVars["PATH"] == "/usr/local/bin:/usr/bin:/bin");
            assert(h.options["00_shell"] == ["sh", "-c"]);
            assert(h.allowedCommandPrefixes == ["ls", "cat", "echo", "pwd"]);
            return true;
        }));
}
