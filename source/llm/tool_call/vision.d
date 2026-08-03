module llm.tool_call.vision;

import logger = std.logger;
import std.algorithm : startsWith;
import std.base64 : Base64;
import std.conv : text;
import std.datetime : Clock;
import std.exception : collectException;
import std.file : getSize, read;
import std.json : JSONValue, JSONType;
import std.path : extension;
import std.string : toLower, strip;
import std.sumtype : SumType, match;
import std.typecons : Nullable;

import my.path : AbsolutePath;

import llm.types : IAgent, AgentProcessResult = ProcessResult;
import llm.summary_agent : SummaryAgent;
import llm.chat : Chat, VisionMessage;
import llm.config : VisionModelConfig, toRequestConfig;
import llm.query : LlmRequester, toJson, LlamaRequestError;
import llm.tool_call;
import llm.tool_call.utility : pathToWorkarea;
import llm.utility : getValue;

mixin RegisterLlmFunctions!();

/// Context interface for vision-related operations.
interface VisionContext : Context {
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();
    bool addVisionImage(AbsolutePath path, string query) nothrow;
    bool hasVisionModel();
    IAgent getVisionAgent();
}

/// Result of loading an image file for vision processing.
struct ImageLoadResult {
    /// The base64-encoded data URL, empty on failure.
    string dataUrl;
    /// Detected MIME type of the image, empty on failure.
    string mimeType;
    /// True if the image was loaded successfully.
    bool success;
    /// Error message when success is false.
    string errorMsg;
}

/// Maximum allowed image file size (20 MB).
private enum size_t MaxImageSize = 20 * 1024 * 1024;

/// Detect MIME type from file extension.
/// Returns empty string if the extension is not a supported image format.
private string mimeTypeFromExtension(string ext) pure {
    switch (ext.toLower) {
    case ".jpg":
    case ".jpeg":
        return "image/jpeg";
    case ".png":
        return "image/png";
    case ".bmp":
        return "image/bmp";
    case ".gif":
        return "image/gif";
    default:
        return "";
    }
}

/// Load an image file and return it as a base64-encoded data URL.
/// Handles MIME detection, file size validation, file reading, and base64 encoding.
/// Returns success:false with an error message on any failure.
ImageLoadResult loadImage(AbsolutePath path) nothrow {
    try {
        const mime = mimeTypeFromExtension(path.extension);
        if (!mime.length) {
            return ImageLoadResult("", "", false, i"unsupported image format: $(path.extension)"
                    .text);
        }

        auto fileSize = getSize(path);
        if (fileSize > MaxImageSize) {
            return ImageLoadResult("", "", false, i"image file too large ($(fileSize) bytes, max $(
                    MaxImageSize))".text);
        }

        string encoded = Base64.encode(cast(const(ubyte)[]) read(path));
        string dataUrl = "data:" ~ mime ~ ";base64," ~ encoded;

        return ImageLoadResult(dataUrl, mime, true, "");
    } catch (Exception e) {
        return ImageLoadResult("", "", false, "failed to load image: " ~ e.msg);
    }
}

/// Default system prompt for the vision agent.
/// Used when VisionModelConfig.systemPrompt is empty.
enum DefaultVisionSystemPrompt = `You are a vision assistant. A user has provided an image and a query.
Describe the image content in detail, focusing on information relevant to the user's query.
Be thorough and objective in your description.`;

/// Lightweight agent for image description using a dedicated vision model.
/// Single-turn: system prompt + image message -> text response.
/// No tool calls, no RAG, no tool contexts.
class DedicatedVisionAgent : IAgent {
    private {
        Chat chat;
        LlmRequester requester;
        string modelName_;
    }

    /// Returns the configured vision model name.
    @property string modelName() const @safe {
        return modelName_;
    }

    /// Create a new vision agent with the given config and system prompt.
    /// When systemPrompt is empty, falls back to DefaultVisionSystemPrompt.
    this(VisionModelConfig config, string systemPrompt) {
        modelName_ = config.name;
        auto prompt = systemPrompt.length == 0 ? DefaultVisionSystemPrompt : systemPrompt;
        chat.setSystemPrompt(prompt);

        auto reqCfg = toRequestConfig(config);
        // toRequestConfig uses server.timeoutSeconds; override with vision-specific timeout
        reqCfg.timeoutS = cast(int) config.timeoutSecs;
        requester = LlmRequester(reqCfg);
    }

    /// IAgent interface: return a unique identifier for this agent.
    override string id() {
        return "vision-agent";
    }

    /// IAgent interface: not applicable for single-turn vision agent.
    /// DedicatedVisionAgent is designed for single-turn image processing only.
    /// Calling this method returns an unknown failure status — callers should use processImage() directly.
    override AgentProcessResult runToCompletion(void delegate(AgentProcessResult) _step = null,
            SummaryAgent.ProgressCallback _compressCallback = null, bool delegate() _interrupt = null) {
        logger.tracef("DedicatedVisionAgent.runToCompletion called - use processImage() instead")
            .collectException;
        return AgentProcessResult(status: AgentProcessResult.Status.unknownFailure);
    }

    static struct Error {
        string msg;
    }

    alias ProcessResult = SumType!(string, Error);

    /// Process an image with the given query and return the text response.
    /// @param imageDataUrl The base64-encoded data URL of the image (data:mime;base64,...)
    /// @param query The user's query about the image
    /// @returns The text response from the vision model
    /// @throws Exception on any failure (network, parsing, etc.)
    ProcessResult processImage(string imageDataUrl, string query) nothrow {
        logger.tracef("Vision processing started: model=%s", modelName_).collectException;
        auto timer = Clock.currTime;

        // Validate input
        if (imageDataUrl.length == 0 || !imageDataUrl.startsWith("data:")) {
            auto duration = Clock.currTime - timer;
            logger.warningf("Vision processing failed after %s: model=%s, reason=invalid image data URL",
                    duration.toString, modelName_).collectException;
            return ProcessResult(Error("Invalid image data URL"));
        }

        // Reset chat to system prompt only (single-turn design).
        // Intentional mutation: each processImage() call starts fresh.
        // The agent is created per-request by AgentContext.getVisionAgent(), so no shared state persists.
        chat.clear();

        // Add user message with image and query
        chat.add(VisionMessage(content: query, imageDataUrl: imageDataUrl));

        ProcessResult result = Error("No response from vision model");
        try {
            // Make synchronous LLM request
            auto jsonResult = requester.request(chat).toJson;

            jsonResult.match!((JSONValue j) {
                auto choices = getValue(j, (v) => v["choices"].array, null);
                if (choices.length == 0) {
                    result = Error("Vision model returned no choices");
                    return;
                }
                foreach (choice; choices) {
                    if (choice.type == JSONType.object && "message" in choice) {
                        auto msg = choice["message"];
                        if ("content" in msg && msg["content"].type == JSONType.string) {
                            result = msg["content"].str.strip;
                            break;
                        }
                    }
                }
            }, (LlamaRequestError e) {
                logger.trace(i"Vision model request failed (code $(e.code)): $(e.response)".text);
                result = Error(i"Vision model request failed (code $(e.code)): $(e.response)".text);
            });
        } catch (Exception e) {
            result = Error("Vision model request failed: " ~ e.msg);
        }

        auto duration = Clock.currTime - timer;
        result.match!((string description) {
            logger.tracef("Vision processing completed: model=%s, duration=%s, responseLength=%d",
                modelName_, duration.toString, description.length).collectException;
        }, (Error err) {
            logger.warningf("Vision processing failed after %s: model=%s, reason=%s",
                duration.toString, modelName_, err.msg).collectException;
        });

        return result;
    }
}

struct LoadImageApiParams {
    @ParamDescription("Path to the image file")
    string path;

    @ParamDescription("Query to include with the image message")
    @ParamOptional string query;
}

// TODO: update supported formats by checking what stb_image supports.
@Function("Load and analyze an image. Returns a text description of the image content. Supported formats: jpg, png, bmp, gif. Use the query to specify what to look for in the image.")
ExecuteFuncResult loadImageApi(Context baseCtx, LoadImageApiParams params) nothrow {
    mixin(baseContextToSpecific!VisionContext);

    try {
        auto path_ = pathToWorkarea(ctx, params.path, checkExist: true);
        if (!path_.valid) {
            return ExecuteFuncResult(path_.errorMsg, success: false);
        }

        auto imageResult = loadImage(path_);
        if (!imageResult.success) {
            return ExecuteFuncResult(imageResult.errorMsg, success: false);
        }

        // Branch: check if dedicated vision model is configured
        if (ctx.hasVisionModel()) {
            // Dedicated path: use vision-specialized model
            // Logging is handled by DedicatedVisionAgent.processImage()
            auto basicAgent = ctx.getVisionAgent();
            auto agent = cast(DedicatedVisionAgent) basicAgent;
            if (agent is null) {
                return ExecuteFuncResult("Vision agent is not a DedicatedVisionAgent",
                        success: false);
            }
            auto result = agent.processImage(imageResult.dataUrl, params.query);
            return result.match!((string description) {
                if (description.length == 0)
                    return ExecuteFuncResult("Vision model returned empty response", success: false);
                return ExecuteFuncResult(description, success: true);
            }, (DedicatedVisionAgent.Error err) {
                return ExecuteFuncResult("Vision processing failed: " ~ err.msg, success: false);
            });
        } else {
            // Inline path: existing behavior - store image for main agent
            if (ctx.addVisionImage(path_, params.query)) {
                logger.tracef("Image loaded for inline processing: path=%s",
                        params.path).collectException;
                return ExecuteFuncResult(i"image loaded from '$(params.path)'".text, success: true);
            }
            logger.warningf("Failed to load image for inline processing: path=%s",
                    params.path).collectException;
            return ExecuteFuncResult(i"error: failed to load image '$(params.path)'".text,
                    success: false);
        }
    } catch (Exception e) {
        return ExecuteFuncResult("error: " ~ e.msg, success: false);
    }
}
