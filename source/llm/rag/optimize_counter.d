/// Auto-optimize write counter operations.
/// The counter holds the number of top-level writes (add, remove, update) since the last optimize run.
module llm.rag.optimize_counter;

import logger = std.experimental.logger;
import miniorm : Miniorm;
import std.exception : collectException;

void incrementOptimizeCounter(ref Miniorm db) nothrow {
    immutable SeedCounterSql = `INSERT INTO OptimizeCounterTbl (count) SELECT 0
        WHERE NOT EXISTS (SELECT 1 FROM OptimizeCounterTbl)`;
    immutable BumpCounterSql = `UPDATE OptimizeCounterTbl SET count = count + 1`;
    try {
        db.run(SeedCounterSql);
        db.run(BumpCounterSql);
    } catch (Exception e) {
        logger.tracef("optimize counter increment skipped: %s", e.msg).collectException;
    }
}

/// The current counter value, 0 when the table is absent (e.g. a read-only open of an older schema).
long readOptimizeCounter(ref Miniorm db) nothrow {
    immutable ReadCounterSql = `SELECT count FROM OptimizeCounterTbl`;

    try {
        auto stmt = db.prepare(ReadCounterSql);
        foreach (ref r; stmt.get.execute) {
            return r.peek!long(0);
        }
        return 0;
    } catch (Exception e) {
        logger.tracef("optimize counter read skipped: %s", e.msg).collectException;
        return 0;
    }
}

void resetOptimizeCounter(ref Miniorm db) nothrow {
    immutable ResetCounterSql = `UPDATE OptimizeCounterTbl SET count = 0`;
    try {
        db.run(ResetCounterSql);
    } catch (Exception e) {
        logger.tracef("optimize counter reset skipped: %s", e.msg).collectException;
    }
}

version (unittest) {
    import llm.rag.sqlite3_vec : SQLITE_OPEN_READWRITE, SQLITE_OPEN_CREATE;

    private Miniorm testCounterDb() {
        return Miniorm(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    }

    private void createCounterTable(ref Miniorm db) {
        db.run(`CREATE TABLE OptimizeCounterTbl (count)`);
    }
}

@("incrementOptimizeCounter seeds the single row and accumulates 1 per call")
unittest {
    import std.conv : to;

    auto db = testCounterDb();
    createCounterTable(db);
    assert(readOptimizeCounter(db) == 0,
            "fresh table reads 0, got " ~ readOptimizeCounter(db).to!string);

    incrementOptimizeCounter(db);
    assert(readOptimizeCounter(db) == 1,
            "one increment reads 1, got " ~ readOptimizeCounter(db).to!string);

    foreach (_; 0 .. 4)
        incrementOptimizeCounter(db);
    assert(readOptimizeCounter(db) == 5,
            "five increments read 5, got " ~ readOptimizeCounter(db).to!string);
}

@("readOptimizeCounter returns 0 when the table is absent and resetOptimizeCounter zeroes it")
unittest {
    import std.conv : to;

    auto db = testCounterDb();
    assert(readOptimizeCounter(db) == 0,
            "absent table must read 0, got " ~ readOptimizeCounter(db).to!string);

    // increment and reset on an absent table must not throw and must stay no-ops
    incrementOptimizeCounter(db);
    assert(readOptimizeCounter(db) == 0,
            "increment without the table must be a no-op, got " ~ readOptimizeCounter(db)
                .to!string);
    resetOptimizeCounter(db);

    createCounterTable(db);
    incrementOptimizeCounter(db);
    assert(readOptimizeCounter(db) == 1,
            "increment must seed and count 1, got " ~ readOptimizeCounter(db).to!string);

    resetOptimizeCounter(db);
    assert(readOptimizeCounter(db) == 0,
            "reset must zero the counter, got " ~ readOptimizeCounter(db).to!string);
}
