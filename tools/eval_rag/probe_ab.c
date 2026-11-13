/*
 * vec0 int8 A/B mechanics probe.
 *
 * Verifies the SQL building blocks the eval_rag 'ab' command relies on,
 * using synthetic 8-dim vectors (no embedder needed):
 *   1. reading a stored float32 vector back out of a vec0 table,
 *   2. quantizing it with vec_quantize_int8(?, 'unit') on insert,
 *   3. KNN MATCH against the int8 table with a pre-quantized blob,
 *   4. KNN MATCH with the quantizer applied inline as the MATCH rhs.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite-vec.h"
#include "sqlite3.h"

static float q0[8] = {1, 0, 0, 0, 0, 0, 0, 0};
static float q1[8] = {0.9f, 0.1f, 0, 0, 0, 0, 0, 0};
static float q2[8] = {0, 0, 0, 0, 0, 0, 1, 0};

static float rows[3][8] = {
    {1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 0},
    {0.8f, 0.6f, 0, 0, 0, 0, 0, 0},
};

static sqlite3* db = NULL;

#define TRY(sql)                                                                                   \
    do {                                                                                           \
        char* err = NULL;                                                                          \
        if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {                                \
            fprintf(stderr, "FAIL exec: %s\n  sql: %s\n", err, sql);                               \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* Runs KNN on the given vec0 table with the given query blob and prints
 * (rowid, distance). */
static int knn(const char* table, const void* vec, int bytes, const char* label) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT id, distance FROM %s WHERE embedding MATCH ? AND k = 3",
             table);
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "FAIL prepare %s: %s\n", label, sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_bind_blob(stmt, 1, vec, bytes, SQLITE_STATIC);
    printf("  %s:", label);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        printf(" (%lld, %.4f)", sqlite3_column_int64(stmt, 0), sqlite3_column_double(stmt, 1));
    if (rc != SQLITE_DONE)
        printf(" [step rc=%d err=%s]", rc, sqlite3_errmsg(db));
    printf("\n");
    sqlite3_finalize(stmt);
    return 0;
}

int main(void) {
    if (sqlite3_open(":memory:", &db) != SQLITE_OK)
        return 1;
    char* errMsg = NULL;
    if (sqlite3_vec_init(db, &errMsg, NULL) != SQLITE_OK) {
        fprintf(stderr, "FAIL vec init: %s\n", errMsg ? errMsg : "?");
        return 1;
    }

    TRY("CREATE VIRTUAL TABLE EvalFloat USING vec0(id INTEGER PRIMARY KEY, sourceId INTEGER NOT "
        "NULL, embedding float[8])");
    TRY("CREATE VIRTUAL TABLE EvalInt8 USING vec0(id INTEGER PRIMARY KEY, sourceId INTEGER NOT "
        "NULL, embedding int8[8])");

    for (int i = 0; i < 3; i++) {
        sqlite3_stmt* ins = NULL;
        if (sqlite3_prepare_v2(db,
                               "INSERT INTO EvalFloat(id, sourceId, embedding) VALUES (?, ?, ?)",
                               -1, &ins, NULL) != SQLITE_OK)
            return 1;
        sqlite3_bind_int64(ins, 1, i + 1);
        sqlite3_bind_int64(ins, 2, 100 + i);
        sqlite3_bind_blob(ins, 3, rows[i], sizeof(rows[i]), SQLITE_STATIC);
        if (sqlite3_step(ins) != SQLITE_DONE)
            return 1;
        sqlite3_finalize(ins);
    }

    /* 1 + 2: copy float -> int8 via the scalar quantizer on insert, keeping
     * the metadata column. */
    TRY("INSERT INTO EvalInt8(id, sourceId, embedding) "
        "SELECT id, sourceId, vec_quantize_int8(embedding, 'unit') FROM EvalFloat");

    /* Plain full scan over the metadata column (id -> sourceId map). */
    {
        sqlite3_stmt* rd = NULL;
        sqlite3_prepare_v2(db, "SELECT id, sourceId FROM EvalInt8", -1, &rd, NULL);
        int n = 0;
        while (sqlite3_step(rd) == SQLITE_ROW)
            n++;
        printf("  metadata full scan rows=%d\n", n);
        sqlite3_finalize(rd);
    }

    /* Read-back sanity: the int8 column returns an 8-byte blob. */
    {
        sqlite3_stmt* rd = NULL;
        sqlite3_prepare_v2(db, "SELECT id, embedding FROM EvalInt8", -1, &rd, NULL);
        while (sqlite3_step(rd) == SQLITE_ROW) {
            const void* blob = sqlite3_column_blob(rd, 1);
            int bytes = sqlite3_column_bytes(rd, 1);
            printf("  readback rowid=%lld bytes=%d first=%d\n", sqlite3_column_int64(rd, 0), bytes,
                   bytes == 8 ? ((const signed char*)blob)[0] : -1);
        }
        sqlite3_finalize(rd);
    }

    /* 3: KNN with a pre-quantized query blob. Quantize q0 via SQL into a
     * buffer, then bind it directly. */
    {
        sqlite3_stmt* q = NULL;
        signed char buf[8];
        sqlite3_prepare_v2(db, "SELECT vec_quantize_int8(vec_f32(?), 'unit')", -1, &q, NULL);
        sqlite3_bind_blob(q, 1, q0, sizeof(q0), SQLITE_STATIC);
        int stepRc = sqlite3_step(q);
        if (stepRc != SQLITE_ROW || sqlite3_column_bytes(q, 0) != 8) {
            fprintf(stderr, "FAIL quantize probe: rc=%d bytes=%d err=%s\n", stepRc,
                    sqlite3_column_bytes(q, 0), sqlite3_errmsg(db));
            return 1;
        }
        memcpy(buf, sqlite3_column_blob(q, 0), 8);
        sqlite3_finalize(q);
        if (knn("EvalInt8", buf, sizeof(buf), "int8 knn, pre-quantized blob, query q0"))
            return 1;
    }

    /* 4: KNN with the quantizer inline as the MATCH rhs. */
    {
        const char* sql = "SELECT id, distance FROM EvalInt8 WHERE embedding MATCH "
                          "vec_quantize_int8(vec_f32(?), 'unit') AND k = 3";
        sqlite3_stmt* stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "FAIL prepare inline: %s\n", sqlite3_errmsg(db));
            return 1;
        }
        sqlite3_bind_blob(stmt, 1, q2, sizeof(q2), SQLITE_STATIC);
        printf("  int8 knn, inline quantize, query q2:");
        while (sqlite3_step(stmt) == SQLITE_ROW)
            printf(" (%lld, %.4f)", sqlite3_column_int64(stmt, 0), sqlite3_column_double(stmt, 1));
        printf("\n");
        sqlite3_finalize(stmt);
    }

    /* Float baseline for the same queries. */
    if (knn("EvalFloat", q0, sizeof(q0), "float knn, query q0"))
        return 1;
    if (knn("EvalFloat", q2, sizeof(q2), "float knn, query q2"))
        return 1;

    printf("ok\n");
    sqlite3_close(db);
    return 0;
}
