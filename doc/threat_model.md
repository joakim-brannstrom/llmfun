# Threat Model: Sandbox Container Configuration System

## Overview

This document describes the threat model for the sandbox container configuration system. The system allows users to configure container runtime options through a flexible options map (`defaultOptions` in `SandboxConfig`, `options` in `ContainerConfig`). These options are flattened into CLI arguments passed to the container runtime (Docker/Podman).

**Security model**: The configuration system operates on a **trust boundary** model. Configuration files are trusted input. The system does not validate the safety of container options — that is the user's responsibility.

## Attacker Model

### Threat Actors

1. **Malicious Agent**: An LLM agent that has been prompted to generate or modify configuration files to escape the sandbox.
2. **Compromised Configuration**: A configuration file that has been tampered with by an external party (e.g., cloned from an untrusted repository).
3. **Insider Threat**: A user who intentionally configures unsafe container options.

### Attack Surface

- Configuration files (`example.yaml`, `.llmfun.yaml`, execution environment files)
- Container runtime CLI arguments
- Mount points and volume mappings
- Magic word substitution (`@{llmfun_workarea}`, `@{llmfun}`)

## Attack Vectors

### 1. Privilege Escalation

**Description**: Attacker injects `--privileged` flag to gain full host access.

**Example**:
```yaml
defaultOptions:
  security: ["--privileged"]
```

**Impact**: Container gains full access to host devices, kernel modules, and can escape sandbox entirely.

**Mitigation**:
- **User responsibility**: Users must review their configuration files.
- System does not validate option safety.

**Related vectors**: Docker capability flags (`--cap-add=ALL`, `--cap-add=NET_ADMIN`, `--cap-add=SYS_ADMIN`) and device access (`--device=/dev/sda`) also grant elevated privileges. Users should audit all security-related options.

### 2. Host Filesystem Access

**Description**: Attacker mounts sensitive host directories into the container.

**Example**:
```yaml
defaultOptions:
  mounts: ["-v", "/etc:/host-etc:rw", "-v", "/:/host-root:rw"]
```

**Impact**: Container can read/write arbitrary host files, including credentials and system configuration.

**Mitigation**:
- **User responsibility**: Users must review mount paths in configuration.
- Magic word substitution only resolves `@{llmfun_workarea}` and `@{llmfun}` — no arbitrary path expansion.

### 3. Network Escape

**Description**: Attacker configures network access to allow the container to communicate with external services.

**Example**:
```yaml
defaultOptions:
  network: ["--network", "host"]
```

**Impact**: Container can access host network, potentially exfiltrate data or attack internal services.

**Mitigation**:
- **User responsibility**: Users must review network configuration.
- Default example uses `"--network", "none"` for isolation.

### 4. Configuration Poisoning

**Description**: Agent writes a `.llmfun.yaml` file in the workarea/CWD that overrides the user's intended configuration.

**Attack Flow**:
1. Agent is prompted to modify configuration
2. Agent writes `.llmfun.yaml` in CWD with unsafe options
3. On next run, the malicious config is loaded, granting the agent more privileges

**Mitigations**:
- **`--no-cwd-config`**: Completely skip loading `.llmfun.yaml` from CWD.
- **`--trusted-config`**: Only load `.llmfun.yaml` from CWD when explicitly allowed. Without this flag, config loading is skipped when `workArea == CWD`.
- Warning is logged when CWD config is skipped.

### 5. Option Injection via Magic Words

**Description**: Attempt to inject arbitrary content through magic word substitution.

**Analysis**:
- `@{llmfun_workarea}` → resolved to workarea absolute path (controlled by user)
- `@{llmfun}` → resolved to binary directory (controlled by system)
- Substitution only applies to **values**, not **keys**
- No recursive substitution or variable expansion

**Impact**: Limited — magic words resolve to fixed, user-controlled paths.

### 6. Denial of Service

**Description**: Malicious configuration causes container runtime to fail or hang.

**Example**:
```yaml
defaultOptions:
  resources: ["--memory", "1b", "--cpus", "0.0001"]
```

**Impact**: Container fails to start or executes extremely slowly.

**Mitigation**: Subprocess timeout (`timeoutSeconds`) limits execution time.

## Mitigations Summary

| Mitigation | Description | Protects Against |
|------------|-------------|------------------|
| `--no-cwd-config` | Skip CWD config entirely | Configuration poisoning |
| `--trusted-config` | Require explicit opt-in for CWD config | Configuration poisoning |
| Execution environments | Curated list of allowed container images | Unauthorized image execution |
| `timeoutSeconds` | Subprocess execution timeout | Denial of service |
| `maxOutputBytes` | Output stream byte limit | Resource exhaustion |
| Magic word scoping | Only two magic words, values only | Option injection |

## User Responsibilities

The user is responsible for:

1. **Reviewing configuration files** before use, especially when cloning from external sources.
2. **Validating container options** for safety (no `--privileged`, no sensitive mounts).
3. **Using `--trusted-config`** when running in directories where the agent can write files.
4. **Maintaining the execution environment files** with only trusted images.
5. **Setting appropriate resource limits** (memory, CPU, timeout).
6. **Using `--no-cwd-config`** in untrusted environments.

## Design Decisions

### Why No Validation?

The system deliberately does not validate container options for the following reasons:

1. **Flexibility**: Different users have different security requirements. What is safe for one environment may be unnecessarily restrictive for another.
2. **Runtime Diversity**: Docker and Podman have different option sets and behaviors. Validating across runtimes is complex and error-prone.
3. **Trust Boundary**: Configuration files are on the trust boundary. Users who can read and modify these files already have significant control over the system.
4. **False Sense of Security**: Partial validation may give users a false sense of security while missing edge cases.

### Tag-Based Keys

Options map keys are **tag names** (logical groups like `"security"`, `"network"`, `"mounts"`), not CLI option names. This design:

- Allows users to organize options logically
- Enables override semantics (image options replace defaults for the same tag)
- Makes configuration more readable and maintainable

## References

- Configuration format: `config/example.yaml`
- Execution environments format: `execution_environments.yaml`
- Implementation: `source/llm/config.d`, `source/llm/environment/`
