// init/deinit of subsystems. Unable to use module init/deinit because the
// subsystems is in separate libraries which do not trigger the module
// constructors.
module llm.subsystem;

version (llmfun_local_model) {
    extern (C) void initLlmfunLocalModel();
    extern (C) void deinitLlmfunLocalModel();
} else {
    extern (C) void initLlmfunLocalModel() {
    }

    extern (C) void deinitLlmfunLocalModel() {
    }
}
