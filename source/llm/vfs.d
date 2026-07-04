// Present a hierarchy of directories as if they are one directory.
// The file that is used is the first one that match the query.

module llm.vfs;

import logger = std.logger;
import std.algorithm : map, filter, joiner;
import std.sumtype : match;
import std.path : baseName, dirName;

import my.path;
import my.optional;

struct FlatVfs {
    import std.array : array;

    private {
        AbsolutePath[] hierarchy;
        string delegate(string) queryFilter;
    }

    this(Path[] hierarchy) {
        this(hierarchy.map!(a => AbsolutePath(a)).array);
    }

    this(AbsolutePath[] hierarchy) {
        this.hierarchy = hierarchy;
    }

    void setQueryFilter(string delegate(string) fn) {
        this.queryFilter = fn;
    }

    Optional!AbsolutePath query(string filename) {
        import std.file : exists;

        if (queryFilter) {
            filename = queryFilter(filename);
        }
        foreach (a; hierarchy.map!(a => a ~ filename)
                .filter!(a => a.exists)) {
            return AbsolutePath(a).some;
        }
        return none!AbsolutePath();
    }

    // Scan the hierarchy in from index 0->last and keep only the unique files
    // found. This mean that if file "foo" is found in hierarchy[0] and in
    // hierarchy[2] the one from 0 is used.
    AbsolutePath[] getAllFiles() @safe {
        import std.file : exists, dirEntries, SpanMode;

        bool[string] found;
        AbsolutePath[] rval;
        foreach (f; hierarchy.filter!(a => a.exists)
                .map!(a => dirEntries(a, SpanMode.shallow))
                .joiner) {
            if (f.name.baseName !in found) {
                rval ~= AbsolutePath(f.name);
                found[f.name.baseName] = true;
            }
        }
        return rval;
    }

    // Read the first matching file in the directory hierarchy.
    Optional!string read(string filename) {
        import std.file : readText;

        return query(filename).match!((AbsolutePath a) => readText(a).some(), (_) => none!string());
    }

    // Write to filename to the first place in the directory hierarchy that has
    // write permission.
    bool save(string filename, string content) {
        import std.file : exists;
        import std.stdio : File;

        if (queryFilter) {
            filename = queryFilter(filename);
        }

        foreach (a; hierarchy.filter!(a => a.exists)
                .map!(a => a ~ filename)) {
            try {
                File(a.toString, "w").write(content);
                return true;
            } catch (Exception e) {
                logger.tracef("failed to write memory to '%s': %s", filename, e.msg);
            }
        }
        return false;
    }

    bool remove(string filename) {
        static import std.file;

        return query(filename).match!((AbsolutePath a) {
            std.file.remove(a);
            return true;
        }, (_) => false);
    }
}
