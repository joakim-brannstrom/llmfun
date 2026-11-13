/* Minimal embedding probe for the vendored llama.cpp fork.
 * Flushes every step so a crash still leaves a trace, and tries both
 * llama_get_embeddings_seq and llama_get_embeddings_ith with the
 * output flag set explicitly on every token.
 */
#include "llama.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct llama_batch make_batch(llama_token* toks, int ntok, llama_pos* pos, int* nSeqId,
                                     llama_seq_id** seqId, int8_t* out) {
    struct llama_batch b;
    memset(&b, 0, sizeof(b));
    b.n_tokens = ntok;
    b.token = toks;
    b.pos = pos;
    b.n_seq_id = nSeqId;
    b.seq_id = seqId;
    b.logits = out; // this fork: "logits" is the output flag array
    return b;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s model.gguf\n", argv[1]);
        return 1;
    }
    ggml_backend_load_all();
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    struct llama_model* m = llama_model_load_from_file(argv[1], mp);
    if (!m) {
        fprintf(stderr, "model load failed\n");
        return 1;
    }

    struct llama_context_params cp = llama_context_default_params();
    cp.embeddings = true;
    cp.pooling_type = LLAMA_POOLING_TYPE_UNSPECIFIED;
    cp.n_ctx = 512;
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    struct llama_context* ctx = llama_init_from_model(m, cp);
    if (!ctx) {
        fprintf(stderr, "ctx failed\n");
        return 1;
    }

    enum llama_pooling_type pt = llama_pooling_type(ctx);
    printf("pooling_type=%d (0=NONE,1=MEAN,2=CLS,3=LAST,4=RANK)\n", (int)pt);
    fflush(stdout);

    const char* text = argc > 2 ? argv[2] : "search_query: I want the file auth_token_spec";
    int cap = 256;
    llama_token* toks = malloc(sizeof(llama_token) * cap);
    llama_pos* pos = malloc(sizeof(llama_pos) * cap);
    int* nSeqId = calloc(cap, sizeof(int));
    llama_seq_id** seqId = calloc(cap, sizeof(llama_seq_id*));
    llama_seq_id* seq0 = malloc(sizeof(llama_seq_id) * cap);
    int8_t* out = calloc(cap, 1);
    int ntok =
        llama_tokenize(llama_model_get_vocab(m), text, (int)strlen(text), toks, cap, false, true);
    printf("ntok=%d\n", ntok);
    fflush(stdout);
    for (int i = 0; i < ntok; i++) {
        pos[i] = i;
        nSeqId[i] = 1;
        seq0[i] = 0;
        seqId[i] = &seq0[i];
        out[i] = 1; // explicit output flag on every token
    }

    struct llama_batch b = make_batch(toks, ntok, pos, nSeqId, seqId, out);
    int rc = llama_encode(ctx, b);
    printf("encode rc=%d\n", rc);
    fflush(stdout);

    const float* eSeq = llama_get_embeddings_seq(ctx, 0);
    printf("get_embeddings_seq(0)=%p\n", (void*)eSeq);
    fflush(stdout);
    const float* eIth = llama_get_embeddings_ith(ctx, 0);
    printf("get_embeddings_ith(0)=%p\n", (void*)eIth);
    fflush(stdout);

    int dim = llama_model_n_embd(m);
    if (eSeq) {
        double norm = 0;
        for (int j = 0; j < dim; j++)
            norm += (double)eSeq[j] * eSeq[j];
        printf("seq emb norm=%.4f\n", sqrt(norm));
    }
    if (eIth) {
        double norm = 0;
        for (int j = 0; j < dim; j++)
            norm += (double)eIth[j] * eIth[j];
        printf("ith emb norm=%.4f\n", sqrt(norm));
    }
    fflush(stdout);
    return 0;
}
