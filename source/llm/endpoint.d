// Endpoint specific functionality
module llm.endpoint;

import llm.config : EndpointType, toRequestConfig;
import llm.query : LlmSlotRequester;

long getContextSize(T)(T conf) {
    final switch (conf.server.toType) with (EndpointType) {
    case unknown:
    case openAiv1:
        return conf.contextSize;
    case llamaCpp:
        auto slot = LlmSlotRequester(conf.toRequestConfig);
        return slot.request(conf.contextSize);
    case deepseek:
        return conf.contextSize;
    }
}
