// RAG implementation using Embedder abstraction.
// Decoupled from specific model implementations via the Embedder interface.

module llm.rag.rag;

import logger = std.logger;
import std.algorithm;
import std.sumtype;
import std.digest;
import std.digest.murmurhash : MurmurHash3;
import std.array : array, empty;
import std.stdio : File;

import my.path;
public import my.path : Path;

import llm.rag.embedder;
import llm.rag.embedder_llama;

struct Unknown {
}

struct Url {
    string value;
}

alias Origin = SumType!(Unknown, Url, Path);

struct Chunk {
    Document doc;
    ulong hash;
    float[] embed;
}

struct Document {
    Origin origin;
    string data;
}

class RAG {
    Embedder embedder;
    Chunk[] chunks;

    /// Primary constructor using Embedder interface.
    this(Embedder embedder) {
        this.embedder = embedder;
    }

    void destroy() {
        embedder.destroy();
    }

    Document[] query(string query, int getTopK) {
        import std.numeric : cosineSimilarity;
        import std.typecons : tuple, Tuple;
        import std.algorithm : schwartzSort;

        auto queryEmbd = embedder.embed(query);
        if (!queryEmbd.has!(float[]))
            return null;
        auto embed = queryEmbd.get!(float[]);
        auto topK = new Tuple!(Chunk, float)[getTopK];
        foreach (ref a; topK)
            a[1] = 0.0;

        foreach (a; chunks.map!(a => tuple(a, cosineSimilarity(embed, a.embed)))) {
            if (a[1] > topK[$ - 1][1]) {
                topK[$ - 1] = a;
                schwartzSort!((a) => a[1], (a, b) => a > b)(topK);
            }
        }
        return topK.map!(a => a[0].doc).array;
    }
}

struct RagAddResult {
    size_t tokens;
    size_t chunks;
}

// Add a document to the RAG.
RagAddResult add(RAG rag, Document doc) {
    uint toUint(ubyte[4] a) {
        return a[0] | a[1] << 8 | a[2] << 16 | a[3] << 24;
    }

    const nBatch = rag.embedder.batchSize();
    auto tokens = rag.embedder.tokenize(doc.data);

    size_t nChunks = 0;
    for (size_t i = 0; i < tokens.length; i += nBatch / 2) {
        auto part = tokens[i .. min(i + nBatch, tokens.length)];
        auto data = rag.embedder.detokenize(part);
        uint h = toUint(digest!(MurmurHash3!32)(data));
        if (rag.chunks.any!((a => a.hash == h))) {
            logger.tracef("Duplicate RAG chunk, skipping. length:%s hash:%s", part.length, h);
        } else {
            rag.embedder.embed(data).match!((float[] embed) {
                rag.chunks ~= Chunk(doc: Document(origin: doc.origin, data: data),
                    hash: h, embed: embed);
            }, (string e) {
                logger.tracef("Failed to generate embedding '%s': %s", e, data);
            });
            nChunks++;
        }
    }

    return RagAddResult(tokens.length, nChunks);
}

void save(ref RAG rag, AbsolutePath filename) {
    import std.json;

    // save embeddings as integers to avoid any loss when converting back and
    // forth between a textual representation of a float.

    uint floatToUint(float v) {
        uint rval;
        ubyte* a = cast(ubyte*)&rval;
        ubyte* b = cast(ubyte*)&v;
        static foreach (i; 0 .. 4) {
            a[i] = b[i];
        }
        return rval;
    }

    logger.tracef("Save RAG to %s. Chunks %s", filename.toString, rag.chunks.length);

    auto f = File(filename, "w");
    foreach (a; rag.chunks) {
        JSONValue j;
        JSONValue oKind;
        JSONValue oValue;

        a.doc.origin.match!((Unknown a) { oKind = "unknown"; }, (Url a) {
            oKind = "url";
            oValue = a.value;
        }, (Path a) { oKind = "path"; oValue = a.toString; });
        auto origin = JSONValue(["kind": oKind, "value": oValue]);
        j["origin"] = origin;
        j["data"] = a.doc.data;
        j["hash"] = a.hash;
        j["embed"] = a.embed.map!(a => floatToUint(a)).array;
        f.writeln(j.toString);
    }
}

void load(RAG rag, AbsolutePath filename) {
    import std.json : JSONValue, parseJSON;
    import std.file : exists;
    import llm.utility : getValue;

    if (!filename.exists) {
        logger.tracef("Load RAG from %s failed. File do not exist", filename);
        return;
    }

    float uintToFloat(uint v) {
        float rval;
        ubyte* a = cast(ubyte*)&rval;
        ubyte* b = cast(ubyte*)&v;
        static foreach (i; 0 .. 4) {
            a[i] = b[i];
        }
        return rval;
    }

    logger.tracef("Load RAG from %s", filename.toString);

    auto f = File(filename, "r");
    string line;
    while (!f.eof()) {
        line = f.readln();
        if (line.empty)
            continue;

        auto j = line.parseJSON;
        Document doc;

        auto kind = getValue(j, (v) => v["origin"].object["kind"].str, "");
        if (kind == "unknown") {
            doc.origin = Unknown();
        } else if (kind == "url") {
            doc.origin = Url(getValue(j, (v) => v["origin"].object["value"].str, ""));
        } else if (kind == "path") {
            doc.origin = getValue(j, (v) => v["origin"].object["value"].str, "").Path;
        }
        doc.data = getValue(j, (v) => v["data"].str, "");

        auto c = Chunk(doc: doc, hash: getValue(j, (v) => v["hash"].integer, 0),
                embed: getValue(j, (v) => v["embed"].array, JSONValue[].init).map!(
                    a => uintToFloat(cast(uint) a.integer)).array);
        rag.chunks ~= c;
    }
}
