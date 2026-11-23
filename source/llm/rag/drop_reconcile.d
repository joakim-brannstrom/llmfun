/// Pure reconcile core for DropRagSync: windowed add/remove planning over a
/// bounded scan slice. No embedder, no database, no actor system.
module llm.rag.drop_reconcile;

import std.datetime.systime : SysTime;
import std.file : DirEntry;
import std.path : buildNormalizedPath, pathSplitter;
import std.range.primitives : ElementType, empty, isInputRange;

import my.path : AbsolutePath, Path;

import llm.rag.rag : Origin, Topic, Url;

/// The observed identity of a file on disk, as the scan provides it for free.
struct FileStamp {
    SysTime mtime;
    ulong size;
}

/// Per-file indexing outcome recorded by the actor in its in-memory map.
enum Outcome {
    /// The file is indexed under its origin (a dedup no-op counts as indexed).
    indexed,
    /// The last indexing attempt failed; `attempts` counts tries toward the poison cap.
    failed,
    /// The caller decided not to index the file (e.g. over the size cap).
    /// With an unchanged stamp it is not re-planned (no per-pass skip spam);
    /// a stamp change re-plans it and the caller re-decides.
    skipped
}

struct FileRecord {
    Outcome outcome;
    FileStamp stamp;
    uint attempts;
    string lastError;
}

/// One bounded tick's plan.
/// Params:
///   K = the scan key form shared by `adds`, the `seen` and `originByPath`
///       keys, and the file origins of `indexed` (`Path` or `AbsolutePath`;
///       see `reconcileWindow`).
struct ReconcilePlan(K = Path) {
    /// Disk paths to index this tick, in the pass's shared scan key form
    /// (normalized; absolute for the DropRagSync actor).
    K[] adds;
    /// Removal candidates decided this tick; the caller accumulates them per
    /// pass, prunes as later windows cover their paths, and applies the
    /// survivors at pass completion (after all adds of the pass).
    Origin[] removes;
    /// True only for the pass's final, empty window. The caller must feed the
    /// empty window exactly once, only when its scan is truly exhausted, and
    /// applies the pending removals in that tick, after all adds of the pass.
    bool passComplete;
}

/// The comparison key for a scanned path or an origin path. Normalization
/// makes the component-wise matching immune to separator and leading-`./`
/// drift between writers; the caller must key `originByPath` with this too.
Path reconcileKey(const Path path) {
    return Path(buildNormalizedPath(path.toString));
}

/// The reconcile key of a file origin, or an empty `Path` for non-file origins.
Path originReconcileKey(const Origin origin) {
    import std.sumtype : match;

    return origin.match!((const Path p) => reconcileKey(p), (_) => Path.init);
}

/// Whether a file origin lies at or under the `destination` prefix, after
/// normalization (the destination itself matches; empty or "." matches nothing).
bool originUnderDestination(const Path origin, const Path destination) {
    import std.algorithm.comparison : equal;
    import std.array : array;

    const dest = buildNormalizedPath(destination.toString);
    if (dest.empty || dest == ".")
        return false;
    auto destParts = pathSplitter(dest).array;
    auto origParts = pathSplitter(buildNormalizedPath(origin.toString)).array;
    return origParts.length >= destParts.length && equal(destParts, origParts[0 .. destParts.length]);
}

/// Plan one tick of the windowed reconcile: which files to add this tick and
/// which indexed origins are removal candidates.
///
/// Params:
///   scanWindow = next slice of file entries from the caller's destination
///       scan. Symlinks and directories must already be excluded. Every
///       entry name must use ONE key form, consistently across the scan, the
///       `seen` and `originByPath` keys, and `indexed` (the DropRagSync
///       actor uses absolute normalized paths because the workarea is not
///       the process CWD; tests use the same form).
///   indexed = the origins under the destination prefix, fetched and
///       pre-filtered by the caller once per pass. They must use the SAME
///       key form as the scan entries: the removal planning compares their
///       `reconcileKey` against the covered scan keys. The caller maps
///       removal candidates back to the stored origins it wrote.
///   seen = the caller's per-file outcome map (empty after a restart).
///   originByPath = the scan-form keys of `indexed` mapped to the stored
///       origins; the `originKnown` check reads its keys.
///   retryCap = failed-attempt poison cap; a file at or above the cap is not
///       re-added until its stamp changes.
///   K = the comparison key type: `Path` (normalized, form-preserving) or
///       `AbsolutePath` (the DropRagSync actor's scan keys). Deduced from
///       the `seen` and `originByPath` map types; every comparison inside
///       the call uses that one form.
///
/// Returns: the tick's plan; the field contracts are documented on
/// `ReconcilePlan`. `adds` holds every scanned path that is new,
/// stamp-changed, missing from `originByPath`, or failed below `retryCap`.
/// A skipped file with an unchanged stamp is not re-added either; a stamp
/// change re-plans it. A failed file at or above `retryCap` with an
/// unchanged stamp is poisoned: never re-attempted until the stamp changes,
/// even when its origin is absent from `originByPath`.
ReconcilePlan!K reconcileWindow(K = Path)(DirEntry[] scanWindow, Origin[] indexed,
        const FileRecord[K] seen, const Origin[K] originByPath, uint retryCap = 3)
        if (is(K == Path) || is(K == AbsolutePath)) {
    import std.sumtype : match;

    ReconcilePlan!K plan;

    // The scan key of a path, in the caller's key form: normalized for
    // `Path`, absolute-normalized for `AbsolutePath`.
    static K keyOf(const Path p) {
        static if (is(K == Path))
            return reconcileKey(p);
        else
            return K(p);
    }

    bool[K] covered;
    foreach (entry; scanWindow) {
        if (entry.isDir)
            continue;
        const path = keyOf(Path(entry.name));
        covered[path] = true;
        const stamp = FileStamp(entry.timeLastModified, entry.size);
        const rec = path in seen;
        const originKnown = (path in originByPath) !is null;
        if (rec !is null && rec.stamp == stamp && (rec.outcome == Outcome.skipped
                || (rec.outcome == Outcome.failed && rec.attempts >= retryCap)))
            continue; // unchanged and settled: skipped or poisoned, no re-attempt
        if (rec is null || rec.stamp != stamp || !originKnown
                || (rec.outcome == Outcome.failed && rec.attempts < retryCap))
            plan.adds ~= path;
    }

    foreach (origin; indexed)
        origin.match!((const Path p) {
            if (!(keyOf(p) in covered))
                plan.removes ~= origin;
        }, (_) {});

    plan.passComplete = scanWindow.length == 0;
    return plan;
}

/// A slice of the recursive destination scan, as one pump tick yields it.
struct ScanSlice {
    /// File entries pulled in this tick, already filtered.
    DirEntry[] entries;
    /// True when `scan` is exhausted: no further entries exist. Only an
    /// exhausted scan may end the pass - an empty `entries` alone does NOT
    /// imply completion (a deadline can expire before any entry is pulled).
    bool exhausted;
}

/// Incremental window feeder over a recursive destination scan, as one pump
/// tick: pulls up to `batchFiles` file entries from `scan`, or until
/// `deadline` passes, whichever first.
///
/// Params:
///   scan = the caller's recursive destination scan. It must implement the
///       walkDrop policy itself (`dragdrop.d:walkDrop`): shallow manual
///       recursion that never follows symlinked directories and skips dot
///       directories. Do NOT use `dirEntries(_, SpanMode.depth)`: depth mode
///       follows symlinked directories, and this function's filter does not
///       stop the underlying range - it only screens the entries it yields.
///   batchFiles = maximum entries pulled per call.
///   deadline = wall-clock cutoff for the pull loop.
///   scanRoot = the path the scan is rooted at, when known. The dot-segment
///       filter applies only to segments BELOW this root: a scan rooted at a
///       path containing dot segments (a dotted CWD-relative workarea, or a
///       user-configured destination like `dropped/.staging`) must still
///       yield its files. Callers rooted at an absolute path MUST pass the
///       root; without a root the filter conservatively applies to the full
///       entry name (correct only for roots without dot segments).
///
/// Returns: the tick's `ScanSlice` (see the struct for the pass-completion
/// contract). Yields only files: dot-prefixed path segments below the scan
/// root (hidden files AND contents of hidden directories such as
/// `dropped/.git/config`), symlinks, and directories are excluded.
ScanSlice nextWindow(R)(ref R scan, size_t batchFiles, SysTime deadline, Path scanRoot = Path.init)
        if (isInputRange!R && is(ElementType!R == DirEntry)) {
    import std.algorithm.iteration : map;
    import std.algorithm.searching : any, startsWith;
    import std.datetime.systime : Clock;
    import std.path : relativePath;
    import std.range.primitives : empty, front, popFront;

    ScanSlice slice;
    while (slice.entries.length < batchFiles && !scan.empty && Clock.currTime < deadline) {
        auto entry = scan.front;
        scan.popFront;
        // Policy-owned segments are the ones below the scan root; without a
        // root, or for an entry not under it, fall back to the full name.
        string name = entry.name;
        if (scanRoot.toString.length) {
            try {
                name = relativePath(entry.name, scanRoot.toString);
            } catch (Exception) {
                // not under the root: keep the full name
            }
        }
        if (pathSplitter(name).any!(seg => seg.startsWith(".")))
            continue;
        if (entry.isSymlink || entry.isDir)
            continue;
        slice.entries ~= entry;
    }
    slice.exhausted = scan.empty;
    return slice;
}

/// Sweep-cadence saturation cap in minutes: at the cap the periodic sweep
/// timer stops until the next drop event restarts it.
enum uint cadenceMinMax = 10;

/// Result of one cadence transition; the decision rule is `cadenceNext`'s.
struct CadenceNext {
    /// the backoff interval for the next completed pass, in minutes
    uint nextMin;
    /// whether to re-arm the cadence timer (false: stop at saturation)
    bool rearm;
}

/// Pure cadence transition: returns the next interval and whether to re-arm.
/// appliedChange: the completed pass applied >= 1 add or remove.
///
/// A pass with applied changes resets the fast edge (1 minute, re-arm); a
/// no-change pass backs off one minute; at the cap the timer stops
/// (`rearm == false`) until the next drop event.
CadenceNext cadenceNext(uint currentMin, bool appliedChange) {
    if (appliedChange)
        return CadenceNext(1, true);
    if (currentMin >= cadenceMinMax)
        return CadenceNext(cadenceMinMax, false);
    return CadenceNext(currentMin + 1, true);
}

version (unittest) {
    import core.time : seconds;
    import std.algorithm.iteration : map;
    import std.array : array;
    import std.conv : to;
    import llm.test_util : TestArea;
    import std.file : mkdirRecurse;
    import std.path : dirName;
    import std.stdio : File;

    /// Force the lazy stat now so the snapshot stays stable across later writes.
    /// (Phobos DirEntry defers stat() to the first property access.)
    private DirEntry snap(string path) {
        auto de = DirEntry(path);
        cast(void) de.size;
        return de;
    }

    /// Create `relPath` under `area` with `content`, return its DirEntry snapshot.
    private DirEntry writeFile(TestArea area, string relPath, string content) {
        const path = area.workArea ~ relPath;
        mkdirRecurse(dirName(path.toString));
        File(path.toString, "w").write(content);
        return snap(path.toString);
    }

    /// A seen-map with one record for `de`, stamped from the entry itself.
    private FileRecord[Path] recordOf(DirEntry de, Outcome outcome, uint attempts = 0) {
        FileRecord[Path] m;
        m[reconcileKey(Path(de.name))] = FileRecord(outcome,
                FileStamp(de.timeLastModified, de.size), attempts);
        return m;
    }

    private Origin[Path] originIndex(Origin[] indexed) {
        Origin[Path] m;
        foreach (origin; indexed)
            m[originReconcileKey(origin)] = origin;
        return m;
    }

    /// The removal candidates' paths, for assertions.
    private string[] removePaths(Origin[] removes) {
        string[] ps;
        foreach (origin; removes)
            ps ~= originReconcileKey(origin).toString;
        return ps;
    }

    /// The planned add paths, for assertions.
    private string[] addPaths(ReconcilePlan!Path plan) {
        return plan.adds.map!(p => p.toString).array;
    }
}

@("nextWindow with a scanRoot applies the dot policy only below the root")
unittest {
    import std.algorithm.iteration : map;
    import std.algorithm.sorting : sort;
    import std.datetime.systime : Clock;
    import llm.test_util : testArea;
    import std.file : dirEntries, SpanMode;
    import std.path : absolutePath, baseName, buildPath;

    // The scan root sits under a dot segment (as it would for a dotted
    // CWD-relative workarea): with the root passed, its files must still
    // be yielded; without a root the conservative full-name filter
    // applies.
    auto area = testArea("next_window_dot_root");
    scope (exit)
        area.cleanup();
    const root = buildPath(area.workArea.toString, ".dotroot", "dropped").absolutePath;
    writeFile(area, ".dotroot/dropped/a.md", "alpha");
    writeFile(area, ".dotroot/dropped/.hidden.txt", "skip me");
    writeFile(area, ".dotroot/dropped/.git/config", "leak");

    const deadline = Clock.currTime + 10.seconds;

    auto scan = dirEntries(root, SpanMode.depth);
    auto rooted = nextWindow(scan, 8, deadline, root.Path);
    assert(rooted.exhausted, "a batch large enough for the tree must exhaust the scan");
    auto names = rooted.entries.map!(e => baseName(e.name)).array.sort.array;
    assert(names == ["a.md"],
            "only the non-hidden file below the root must be yielded, got " ~ names.to!string);

    auto scan2 = dirEntries(root, SpanMode.depth);
    auto unrooted = nextWindow(scan2, 8, deadline);
    assert(unrooted.exhausted, "a batch large enough for the tree must exhaust the scan");
    assert(unrooted.entries.length == 0,
            "without a root the dot filter conservatively applies to the full name");
}

@("reconcile adds a new file and plans removal of a gone file") unittest {
    import llm.test_util : testArea;

    auto area = testArea("reconcile_add_and_remove");
    scope (exit)
        area.cleanup();
    auto deA = writeFile(area, "dropped/a.md", "alpha");

    auto aKey = reconcileKey(Path(deA.name));
    auto goneKey = Path("dropped/gone.md");
    Origin[] indexed = [Origin(Path(deA.name)), Origin(goneKey)];

    FileRecord[Path] seen;
    auto plan = reconcileWindow([deA], indexed, seen, originIndex(indexed));

    assert(addPaths(plan) == [aKey.toString], "the existing file must be planned as an add");
    assert(removePaths(plan.removes) == [goneKey.toString],
            "the gone file's origin must be a removal candidate");
    assert(!plan.passComplete, "a non-empty window is not the end of the pass");
}

@("reconcile skips unchanged indexed content") unittest {
    import llm.test_util : testArea;

    auto area = testArea("reconcile_skip_unchanged");
    scope (exit)
        area.cleanup();
    auto deA = writeFile(area, "dropped/a.md", "alpha");
    Origin[] indexed = [Origin(Path(deA.name))];

    auto plan = reconcileWindow([deA], indexed, recordOf(deA, Outcome.indexed),
            originIndex(indexed));

    assert(plan.adds.empty, "unchanged indexed content must be skipped");
    assert(plan.removes.empty, "a file present in the window must not be a removal candidate");
    assert(!plan.passComplete);
}

@("reconcile re-adds on a changed stamp") unittest {
    import llm.test_util : testArea;

    auto area = testArea("reconcile_readd_changed_stamp");
    scope (exit)
        area.cleanup();
    auto before = writeFile(area, "dropped/a.md", "one");
    const p = (area.workArea ~ "dropped/a.md").toString;
    File(p, "a").write("-and-more");
    auto after = snap(p);
    assert(before.size != after.size, "the append must change the stamp");
    Origin[] indexed = [Origin(Path(after.name))];

    auto plan = reconcileWindow([after], indexed, recordOf(before,
            Outcome.indexed), originIndex(indexed));

    assert(addPaths(plan) == [reconcileKey(Path(after.name)).toString],
            "a changed stamp must re-add the file");
}

@("reconcile heals an out-of-band origin removal") unittest {
    import llm.test_util : testArea;

    auto area = testArea("reconcile_heal_out_of_band");
    scope (exit)
        area.cleanup();
    auto deA = writeFile(area, "dropped/a.md", "alpha");
    Origin[] indexed = [Origin(Path("dropped/other.md"))];

    auto plan = reconcileWindow([deA], indexed, recordOf(deA, Outcome.indexed),
            originIndex(indexed));

    assert(addPaths(plan) == [reconcileKey(Path(deA.name)).toString],
            "an indexed file whose origin is gone must be re-added");
}

@("reconcile retries a transient failure under the cap") unittest {
    import llm.test_util : testArea;

    auto area = testArea("reconcile_retry_transient");
    scope (exit)
        area.cleanup();
    auto deA = writeFile(area, "dropped/a.md", "alpha");
    Origin[] indexed = [Origin(Path(deA.name))];

    foreach (attempts; [1u, 2u]) {
        auto plan = reconcileWindow([deA], indexed, recordOf(deA,
                Outcome.failed, attempts), originIndex(indexed));
        assert(addPaths(plan) == [reconcileKey(Path(deA.name)).toString],
                "a failure under the cap must be retried");
    }
}

@("reconcile poisons a persistent failure at the cap") unittest {
    import llm.test_util : testArea;

    auto area = testArea("reconcile_poison_at_cap");
    scope (exit)
        area.cleanup();
    auto deA = writeFile(area, "dropped/a.md", "alpha");
    Origin[] indexed = [Origin(Path(deA.name))];

    auto plan = reconcileWindow([deA], indexed, recordOf(deA, Outcome.failed,
            3), originIndex(indexed));

    assert(plan.adds.empty, "a failure at the cap must not be retried");
    assert(plan.removes.empty, "a poisoned but existing file must not be a removal candidate");
}

@("reconcile clears poison when the stamp changes") unittest {
    import llm.test_util : testArea;

    auto area = testArea("reconcile_unpoison_on_stamp");
    scope (exit)
        area.cleanup();
    auto before = writeFile(area, "dropped/a.md", "one");
    const p = (area.workArea ~ "dropped/a.md").toString;
    File(p, "a").write("-and-more");
    auto after = snap(p);
    Origin[] indexed = [Origin(Path(after.name))];

    auto plan = reconcileWindow([after], indexed, recordOf(before,
            Outcome.failed, 3), originIndex(indexed));

    assert(addPaths(plan) == [reconcileKey(Path(after.name)).toString],
            "a stamp change must clear the poison and re-add");
}

@("a windowed pass equals a single-window pass") unittest {
    import std.algorithm.iteration : filter, map;
    import std.algorithm.searching : canFind;
    import std.algorithm.sorting : sort;
    import llm.test_util : testArea;

    auto area = testArea("windowed_pass_single_window");
    scope (exit)
        area.cleanup();
    auto deA = writeFile(area, "dropped/a.md", "alpha");
    auto deB = writeFile(area, "dropped/b.md", "beta");
    auto deC = writeFile(area, "dropped/sub/c.md", "gamma");
    DirEntry[] all = [deA, deB, deC];

    auto goneKey = Path("dropped/gone.md");
    Origin[] indexed = all.map!(e => Origin(Path(e.name))).array ~ Origin(goneKey);

    FileRecord[Path] seen;
    auto originByPath = originIndex(indexed);

    // Reference: the whole tree as one window.
    auto one = reconcileWindow(all, indexed, seen, originByPath);
    assert(one.adds.length == 3, "every scanned file must be an add after a restart");
    assert(removePaths(one.removes) == [goneKey.toString],
            "only the gone origin is a removal in the single-window pass");

    // Windowed: three windows plus the final empty window, with the caller
    // contract simulated (update seen, accumulate candidates, prune covered).
    FileRecord[Path] seenMutable;
    bool[Path] covered;
    string[] pending;
    bool[] completes;
    string[] windowedAdds;
    foreach (window; [all[0 .. 1], all[1 .. 2], all[2 .. $]]) {
        auto plan = reconcileWindow(window, indexed, seenMutable, originByPath);
        completes ~= plan.passComplete;
        foreach (entry; window) {
            const key = reconcileKey(Path(entry.name));
            covered[key] = true;
            if (plan.adds.canFind(key))
                seenMutable[key] = FileRecord(Outcome.indexed,
                        FileStamp(entry.timeLastModified, entry.size));
        }
        // The caller must re-prune pending candidates each tick: a later
        // window may cover a path an earlier tick flagged.
        pending = pending.filter!(k => (Path(k) in covered) is null).array;
        windowedAdds ~= plan.adds.map!(p => p.toString).array;
        foreach (origin; plan.removes) {
            const key = originReconcileKey(origin).toString;
            if (key.length && !(Path(key) in covered) && !pending.canFind(key))
                pending ~= key;
        }
        pending = pending.sort.array;
    }
    auto finalPlan = reconcileWindow([], indexed, seenMutable, originByPath);
    assert(finalPlan.passComplete, "the empty final window completes the pass");
    assert(finalPlan.adds.empty, "the final empty window plans no adds");
    foreach (origin; finalPlan.removes) {
        const key = originReconcileKey(origin).toString;
        if (key.length && !(Path(key) in covered) && !pending.canFind(key))
            pending ~= key;
    }
    pending = pending.sort.array;

    assert(completes.length == 3 && !completes[0] && !completes[1]
            && !completes[2], "no intermediate tick completes the pass");
    assert(pending == [goneKey.toString],
            "the windowed pass must prune candidates whose file a later window covers");
    assert(windowedAdds.sort.array == one.adds.map!(p => p.toString)
            .array.sort.array, "the windowed pass must plan the same union of adds");
}

@("identical-content deletion self-heals: twin add precedes the stale remove") unittest {
    import llm.test_util : testArea;

    auto area = testArea("twin_add_before_stale_remove");
    scope (exit)
        area.cleanup();
    // A and B once collapsed into one source under origin A (first wins);
    // A has since been deleted, the identical twin B survives.
    auto deB = writeFile(area, "dropped/b.md", "same");
    const staleKey = Path("dropped/a.md");
    Origin[] indexed = [Origin(staleKey)];

    auto plan = reconcileWindow([deB], indexed, recordOf(deB, Outcome.indexed),
            originIndex(indexed));

    assert(addPaths(plan) == [reconcileKey(Path(deB.name)).toString],
            "the surviving twin must be re-added under its own origin");
    assert(removePaths(plan.removes) == [staleKey.toString], "the stale origin must be removed");
    assert(plan.adds.length == 1 && plan.removes.length == 1,
            "adds precede removes in the plan; the caller applies them in that order");
}

@("originUnderDestination matches component-wise") unittest {
    assert(originUnderDestination(Path("dropped/a.md"), Path("dropped")));
    assert(originUnderDestination(Path("dropped/sub/a.md"), Path("dropped")));
    assert(originUnderDestination(Path("dropped/sub/a.md"), Path("dropped/sub")));
    assert(originUnderDestination(Path("./dropped/a.md"), Path("dropped")),
            "leading ./ must normalize away");
    assert(originUnderDestination(Path("dropped"), Path("dropped")),
            "the destination itself counts as under (prefix semantics)");
    assert(!originUnderDestination(Path("droppedX/a.md"), Path("dropped")),
            "a raw string prefix must not match");
    assert(!originUnderDestination(Path("other/a.md"), Path("dropped")));
    assert(!originUnderDestination(Path("dropped/sub/a.md"), Path("dropped/other")));
    assert(!originUnderDestination(Path("/dropped/a.md"), Path("dropped")),
            "an absolute origin must not match a relative destination");
    assert(!originUnderDestination(Path("dropped/a.md"), Path("")),
            "an empty destination matches nothing");
}

@("nextWindow slices, and skips dot entries, symlinks and directories") unittest {
    import std.algorithm.iteration : joiner, map;
    import std.algorithm.sorting : sort;
    import std.datetime.systime : Clock;
    import llm.test_util : testArea;
    import std.file : dirEntries, SpanMode, symlink;
    import std.path : baseName, buildPath;

    auto area = testArea("next_window_slices");
    scope (exit)
        area.cleanup();
    writeFile(area, "dropped/a.md", "alpha");
    writeFile(area, "dropped/.hidden.txt", "skip me");
    writeFile(area, "dropped/sub/b.md", "beta");
    symlink(buildPath(area.workArea.toString, "dropped", "a.md"),
            buildPath(area.workArea.toString, "dropped", "link.md"));

    auto scan = dirEntries(buildPath(area.workArea.toString, "dropped"), SpanMode.depth);
    const deadline = Clock.currTime + 10.seconds;

    ScanSlice[] slices;
    while (!scan.empty) {
        auto slice = nextWindow(scan, 2, deadline);
        assert(slice.entries.length <= 2, "a window must respect the batch size");
        slices ~= slice;
    }
    assert(slices.length >= 2, "a multi-file tree must need more than one slice");
    assert(slices[$ - 1].exhausted, "the last slice must report the exhausted scan");

    auto names = slices.map!(s => s.entries)
        .joiner
        .map!(e => baseName(e.name))
        .array
        .sort
        .array;
    assert(names == ["a.md", "b.md"],
            "exactly the regular files must be yielded, got " ~ names.to!string);
}

@("nextWindow excludes dot-directory contents and reports deadline cuts") unittest {
    import std.algorithm.iteration : joiner, map;
    import std.algorithm.sorting : sort;
    import std.datetime.systime : Clock;
    import llm.test_util : testArea;
    import std.file : dirEntries, SpanMode;
    import std.path : buildPath;

    auto area = testArea("next_window_dotdir_deadline");
    scope (exit)
        area.cleanup();
    writeFile(area, "dropped/keep.md", "mine");
    // Contents of a hidden directory must never leak into the index, even
    // though their own base names carry no dot.
    writeFile(area, "dropped/.git/config", "leak");

    auto scan = dirEntries(buildPath(area.workArea.toString, "dropped"), SpanMode.depth);
    const deadline = Clock.currTime + 10.seconds;

    auto slice = nextWindow(scan, 8, deadline);
    assert(slice.exhausted, "a batch large enough for the tree must exhaust the scan");
    assert(slice.entries.length == 1,
            "exactly one file must survive the filters, got " ~ slice.entries.length.to!string);

    import std.path : baseName;

    assert(baseName(slice.entries[0].name) == "keep.md",
            "dot-directory contents must be excluded, got " ~ slice.entries[0].name);

    // A deadline that has already passed yields an empty slice WITHOUT
    // marking the scan exhausted, and consumes nothing: the caller must keep
    // pumping instead of treating the tick as pass completion.
    auto scan2 = dirEntries(buildPath(area.workArea.toString, "dropped"), SpanMode.depth);
    const past = Clock.currTime;
    auto cut = nextWindow(scan2, 8, past);
    assert(cut.entries.empty, "a past deadline must yield no entries");
    assert(!cut.exhausted, "a deadline-cut slice must not claim exhaustion");
    assert(!scan2.empty, "a deadline-cut tick must not consume the scan");

    // After the cut the pump resumes: the same file is still there.
    auto resumed = nextWindow(scan2, 8, deadline);
    assert(resumed.entries.length == 1 && resumed.exhausted,
            "the pump must resume cleanly after a deadline cut");
}

@("non-file origins are inert in the reconcile plan") unittest {
    import llm.test_util : testArea;

    auto area = testArea("nonfile_origins_inert");
    scope (exit)
        area.cleanup();
    auto de = writeFile(area, "dropped/a.md", "alpha");

    const goneKey = Path("dropped/gone.md");
    Origin[] indexed = [
        Origin(Topic("some_topic")), Origin(Url("https://example.com")),
        Origin(goneKey)
    ];

    auto plan = reconcileWindow([de], indexed, null, originIndex(indexed));

    assert(addPaths(plan) == [reconcileKey(Path(de.name)).toString],
            "non-file origins must not disturb the add planning");
    assert(removePaths(plan.removes) == [goneKey.toString],
            "only the file origin may become a removal candidate");
}

@("a skipped file with an unchanged stamp is not re-added; a stamp change re-plans it")
unittest {
    import llm.test_util : testArea;

    auto area = testArea("skipped_stamp_replan");
    scope (exit)
        area.cleanup();
    auto before = writeFile(area, "dropped/big.md", "one");
    const p = (area.workArea ~ "dropped/big.md").toString;
    Origin[] indexed = [Origin(Path(before.name))];
    auto originByPath = originIndex(indexed);

    auto plan = reconcileWindow([before], indexed, recordOf(before,
            Outcome.skipped), originByPath);
    assert(plan.adds.empty, "a skipped file with an unchanged stamp must not be re-added");
    assert(plan.removes.empty, "a file present in the window must not be a removal candidate");

    File(p, "a").write("-and-more");
    auto after = snap(p);
    auto plan2 = reconcileWindow([after], indexed, recordOf(before,
            Outcome.skipped), originByPath);
    assert(addPaths(plan2) == [reconcileKey(Path(after.name)).toString],
            "a stamp change must re-plan the skipped file");
}

@("cadenceNext backs off to saturation, stops there, and resets on a change")
unittest {
    // A no-change pass backs off one minute per completed pass...
    auto c = cadenceNext(1u, false);
    assert(c.nextMin == 2 && c.rearm);
    c = cadenceNext(9u, false);
    assert(c.nextMin == 10 && c.rearm, "the pass that reaches the cap still re-arms");

    // ...until the cap, where the timer stops (no re-arm)...
    c = cadenceNext(10u, false);
    assert(c.nextMin == 10 && !c.rearm, "saturation must stop the timer");

    // ...and an applied change resets the fast edge with a re-arm.
    c = cadenceNext(10u, true);
    assert(c.nextMin == 1 && c.rearm);
    c = cadenceNext(7u, true);
    assert(c.nextMin == 1 && c.rearm);
}
