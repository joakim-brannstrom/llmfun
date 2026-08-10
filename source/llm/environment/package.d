/// Public API for the environment module.
/// Re-exports types from config, backend, and dispatch submodules.
module llm.environment;

public import llm.environment.config : ExecutionType, CommandJoinMode,
    ContainerConfig, HostConfig, EnvironmentBackend, loadExecutionBackends;
public import llm.environment.backend : ExecutionResult, RunnerBackend,
    ContainerRunner, HostRunner, collectOutputLimited;
public import llm.environment.dispatch : EnvironmentContext, executeCommand, listEnvironments;
