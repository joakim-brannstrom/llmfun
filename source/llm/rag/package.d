module llm.rag;

public import llm.rag.database;
public import llm.common.embedder;
public import llm.rag.rag;

shared static this() {
    registerEmbedderFactory("remote", &createEmbedder);
}

import std.sumtype : match;
import llm.common.config : EmbedConfig, LocalEmbedConfig, RemoteEmbedConfig;
import llm.common.embedder : Embedder, registerEmbedderFactory;

Embedder createEmbedder(EmbedConfig config) {
    import llm.rag.embedder_http : RemoteEmbedder;

    return config.match!((RemoteEmbedConfig a) { return new RemoteEmbedder(a); },
            (LocalEmbedConfig _) => null);
}
