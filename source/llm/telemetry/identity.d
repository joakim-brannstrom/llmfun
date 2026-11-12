module llm.telemetry.identity;

import logger = std.logger;
import std.array : empty;
import std.exception : collectException;

import llm.config : ProgramName, appVersion;

/// The event name of the startup record.
enum startupEventName = "startup";

/// One interface address candidate from getifaddrs. A candidate carries
/// at most one address (hasV4 xor hasV6; entries without an address are
/// dropped, never stored).
struct IpCandidate {
    int ifIndex;
    string ifName;
    /// IPv4 octets in network byte order, meaningful only when hasV4.
    ubyte[4] v4;
    /// IPv6 bytes in network byte order, meaningful only when hasV6.
    ubyte[16] v6;
    bool hasV4;
    bool hasV6;
}

/// IPv4 is reportable unless it is loopback (127.0.0.0/8) or link-local
/// (169.254.0.0/16), neither of which identifies the host to the outside.
bool isReportableAddress(const ubyte[4] v4) pure @safe @nogc nothrow {
    return v4[0] != 127 && !(v4[0] == 169 && v4[1] == 254);
}

/// IPv6 is reportable only if global unicast: not the unspecified address
/// ::, not loopback ::1, not multicast ff00::/8, not link-local fe80::/10,
/// not ULA fc00::/7 (ULAs are site-local and do not identify the host
/// beyond its own site). getifaddrs reports multicast group memberships as
/// AF_INET6 entries, so the multicast case is not hypothetical.
bool isReportableAddress(const ubyte[16] v6) pure @safe @nogc nothrow {
    size_t zeroBytes;
    foreach (b; v6) {
        if (b == 0)
            ++zeroBytes;
    }
    if (zeroBytes == 16)
        return false; // unspecified ::
    if (zeroBytes == 15 && v6[15] == 1)
        return false; // loopback ::1
    if (v6[0] == 0xff)
        return false; // multicast ff00::/8 is never a host address
    if (v6[0] == 0xfe && (v6[1] & 0xc0) == 0x80)
        return false; // fe80::/10
    if ((v6[0] & 0xfe) == 0xfc)
        return false; // fc00::/7
    return true;
}

/// Pinned selection policy over a candidate list: reportable candidates
/// only, IPv4 preferred over IPv6, then lowest ifIndex, then
/// lexicographically smallest ifName. An empty or fully unreportable list
/// selects "unknown".
string selectHostIp(const IpCandidate[] candidates) pure @safe nothrow {
    string bestIp = "unknown";
    int bestClass = int.max;
    int bestIndex;
    string bestName;

    foreach (const ref c; candidates) {
        if (c.hasV4 && isReportableAddress(c.v4) && isBetter(0, c.ifIndex,
                c.ifName, bestClass, bestIndex, bestName)) {
            bestClass = 0;
            bestIndex = c.ifIndex;
            bestName = c.ifName;
            bestIp = renderV4(c.v4);
        }
        if (c.hasV6 && isReportableAddress(c.v6) && isBetter(1, c.ifIndex,
                c.ifName, bestClass, bestIndex, bestName)) {
            bestClass = 1;
            bestIndex = c.ifIndex;
            bestName = c.ifName;
            bestIp = renderV6(c.v6);
        }
    }
    return bestIp;
}

/// True when a candidate beats the current best: a lower class wins (IPv4
/// over IPv6), then a lower interface index, then the smaller name.
private bool isBetter(int cls, int idx, string name, int bestCls, int bestIdx, string bestName) pure @safe @nogc nothrow {
    if (cls != bestCls)
        return cls < bestCls;
    if (idx != bestIdx)
        return idx < bestIdx;
    return name < bestName;
}

/// The startup record payload: a single-line JSON object (newlines in
/// the identity data come out escaped, so the syslog MSG is never split).
string startupMessage(string timestamp, string host, string ip, string user) @safe {
    import std.json : JSONValue, toJSON;

    JSONValue j;
    j["timestamp"] = timestamp;
    j["host"] = host;
    j["ip"] = ip;
    j["userId"] = user;
    j["applicationName"] = ProgramName;
    j["version"] = appVersion;
    j["eventName"] = startupEventName;
    return toJSON(j);
}

/// getifaddrs -> filter -> selectHostIp. Any failure (getifaddrs error,
/// unusable candidate list) returns "unknown", logged at trace level only.
string hostIp() @safe {
    auto collected = collectCandidates();
    if (!collected.ok) {
        logger.tracef("telemetry identity: getifaddrs failed, reporting host=unknown")
            .collectException;
        return "unknown";
    }
    auto ip = selectHostIp(collected.candidates);
    if (ip == "unknown") {
        logger.tracef("telemetry identity: no reportable interface address, reporting host=unknown")
            .collectException;
    }
    return ip;
}

/// gethostname -> host name; "unknown" on failure, logged at trace level
/// only.
string hostName() @safe {
    auto name = posixHostName();
    if (!name.empty)
        return name;

    logger.tracef("telemetry identity: gethostname returned no name, reporting host=unknown")
        .collectException;
    return "unknown";
}

/// getpwuid(getuid()) -> pw_name; USER, then LOGNAME, environment fallback
/// only when getpwuid returns nothing; else "unknown". Failures logged at
/// trace level only.
string userName() @safe {
    auto name = passwdUserName();
    if (!name.empty)
        return name;

    logger.tracef("telemetry identity: getpwuid returned no user name, trying the environment")
        .collectException;

    import std.process : environment;

    name = environment.get("USER");
    if (!name.empty)
        return name;
    name = environment.get("LOGNAME");
    if (!name.empty)
        return name;

    logger.tracef("telemetry identity: no user name from getpwuid, USER or LOGNAME, reporting user=unknown")
        .collectException;
    return "unknown";
}

/// @trusted bridge over getpwuid: pw_name points into a static passwd
/// struct that any later lookup overwrites, so the name is copied into a
/// D string immediately and the libc pointers never escape.
private @trusted string passwdUserName() nothrow {
    import core.sys.posix.pwd : getpwuid;
    import core.sys.posix.unistd : getuid;
    import std.string : fromStringz;

    auto pw = getpwuid(getuid());
    if (pw is null || pw.pw_name is null)
        return null;
    return pw.pw_name.fromStringz.idup;
}

/// @trusted bridge over gethostname
/// Returns: Empty when the call fails
private @trusted string posixHostName() nothrow {
    import core.sys.posix.unistd : gethostname;
    import std.string : fromStringz;

    // 256 bytes: POSIX requires at least 255, and the last byte is forced to
    // NUL so a libc that truncates without terminating can never send
    // fromStringz past the buffer.
    char[256] buf;
    buf[$ - 1] = 0;
    if (gethostname(buf.ptr, buf.length) != 0)
        return null;
    return buf.ptr.fromStringz.idup;
}

private struct CollectedAddresses {
    IpCandidate[] candidates;
    bool ok;
}

/// @trusted bridge over getifaddrs/freeifaddrs: reads the libc list once
/// and copies every address (family tag plus raw bytes) and interface name
/// out into D-owned candidates. The libc pointers never escape. ok is
/// false only when the getifaddrs call itself failed.
private @trusted CollectedAddresses collectCandidates() nothrow {
    import core.sys.linux.ifaddrs : ifaddrs, getifaddrs, freeifaddrs;
    import core.sys.posix.net.if_ : if_nametoindex;
    import core.sys.posix.sys.socket : AF_INET, AF_INET6;
    import std.string : fromStringz;

    ifaddrs* list;
    if (getifaddrs(&list) != 0)
        return CollectedAddresses.init;
    scope (exit)
        freeifaddrs(list);

    CollectedAddresses result;
    for (auto it = list; it !is null; it = it.ifa_next) {
        if (it.ifa_addr is null)
            continue;

        // sockaddr ABI: sa_family is a ushort at offset 0, the IPv4 address
        // bytes sit at offsets 4..8 and the IPv6 address bytes at 8..24.
        // AF_PACKET and similar entries are skipped before any name lookup.
        const family = it.ifa_addr.sa_family;
        if (family != AF_INET && family != AF_INET6)
            continue;

        IpCandidate c;
        c.ifName = it.ifa_name.fromStringz.idup;
        c.ifIndex = cast(int) if_nametoindex(it.ifa_name);

        const base = cast(const(ubyte)*) it.ifa_addr;
        if (family == AF_INET) {
            c.hasV4 = true;
            foreach (i; 0 .. 4)
                c.v4[i] = base[4 + i];
        } else {
            c.hasV6 = true;
            foreach (i; 0 .. 16)
                c.v6[i] = base[8 + i];
        }
        result.candidates ~= c;
    }
    result.ok = true;
    return result;
}

/// Dotted quad rendered by hand so the helper stays pure and allocation-
/// free apart from the result string.
private string renderV4(const ubyte[4] v4) pure @safe nothrow {
    char[15] buf;
    size_t n;
    foreach (i, o; v4) {
        if (i != 0)
            buf[n++] = '.';
        n += putDecimal(buf[n .. $], o);
    }
    return buf[0 .. n].idup;
}

private size_t putDecimal(char[] dst, const ubyte value) pure @safe @nogc nothrow {
    size_t n;
    if (value == 0) {
        dst[n++] = '0';
    } else {
        char[3] tmp;
        size_t m;
        for (size_t v = value; v > 0; v /= 10) {
            tmp[m++] = cast(char)('0' + v % 10);
        }
        foreach_reverse (d; tmp[0 .. m])
            dst[n++] = d;
    }
    return n;
}

/// IPv6 rendering: eight uncompressed lowercase hex groups separated by
/// ':' (non-canonical form is acceptable here, so ::1 renders as
/// 0000:0000:0000:0000:0000:0000:0000:0001).
private string renderV6(const ubyte[16] v6) pure @safe nothrow {
    char[39] buf;
    size_t n;
    foreach (g; 0 .. 8) {
        if (g != 0)
            buf[n++] = ':';
        buf[n++] = hexDigit(v6[g * 2] >> 4);
        buf[n++] = hexDigit(v6[g * 2] & 0x0f);
        buf[n++] = hexDigit(v6[g * 2 + 1] >> 4);
        buf[n++] = hexDigit(v6[g * 2 + 1] & 0x0f);
    }
    return buf.idup;
}

private char hexDigit(size_t nibble) pure @safe @nogc nothrow {
    return cast(char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

@("IPv4 loopback and link-local addresses are not reportable") unittest {
    assert(!isReportableAddress(cast(const ubyte[4])[127, 0, 0, 1]));
    assert(!isReportableAddress(cast(const ubyte[4])[127, 200, 1, 9]));
    assert(!isReportableAddress(cast(const ubyte[4])[169, 254, 12, 34]));
    assert(isReportableAddress(cast(const ubyte[4])[192, 168, 1, 10]));
    assert(isReportableAddress(cast(const ubyte[4])[8, 8, 8, 8]));
    assert(isReportableAddress(cast(const ubyte[4])[10, 1, 2, 3]));
}

@("IPv6 unspecified, loopback, multicast, link-local and ULA addresses are not reportable") unittest {
    ubyte[16] loopback;
    loopback[15] = 1;
    assert(!isReportableAddress(loopback));

    ubyte[16] linkLocal;
    linkLocal[0] = 0xfe;
    linkLocal[1] = 0x80;
    linkLocal[15] = 1;
    assert(!isReportableAddress(linkLocal));

    ubyte[16] ulaFc;
    ulaFc[0] = 0xfc;
    ulaFc[15] = 1;
    assert(!isReportableAddress(ulaFc));

    ubyte[16] ulaFd;
    ulaFd[0] = 0xfd;
    ulaFd[15] = 1;
    assert(!isReportableAddress(ulaFd));

    ubyte[16] unspecified;
    assert(!isReportableAddress(unspecified));

    ubyte[16] multicast;
    multicast[0] = 0xff;
    multicast[1] = 0x02;
    multicast[15] = 1;
    assert(!isReportableAddress(multicast));

    ubyte[16] global_;
    global_[0] = 0x20;
    global_[1] = 0x01;
    global_[2] = 0x0d;
    global_[3] = 0xb8;
    global_[15] = 1;
    assert(isReportableAddress(global_));
}

@("selectHostIp prefers IPv4 over IPv6 regardless of index or name") unittest {
    IpCandidate v6Best;
    v6Best.ifIndex = 1;
    v6Best.ifName = "eth0";
    v6Best.v6[0] = 0x20;
    v6Best.v6[1] = 0x01;
    v6Best.v6[2] = 0x0d;
    v6Best.v6[3] = 0xb8;
    v6Best.v6[15] = 1;
    v6Best.hasV6 = true;

    IpCandidate v4;
    v4.ifIndex = 5;
    v4.ifName = "eth1";
    v4.v4 = [192, 168, 1, 10];
    v4.hasV4 = true;

    assert(selectHostIp([v6Best, v4]) == "192.168.1.10");
    assert(selectHostIp([v4, v6Best]) == "192.168.1.10");
}

@("selectHostIp breaks ties on the lowest ifIndex first") unittest {
    IpCandidate lowNameHighIndex;
    lowNameHighIndex.ifIndex = 3;
    lowNameHighIndex.ifName = "eth1";
    lowNameHighIndex.v4 = [10, 0, 0, 1];
    lowNameHighIndex.hasV4 = true;

    IpCandidate highNameLowIndex;
    highNameLowIndex.ifIndex = 2;
    highNameLowIndex.ifName = "eth9";
    highNameLowIndex.v4 = [10, 0, 0, 9];
    highNameLowIndex.hasV4 = true;

    assert(selectHostIp([lowNameHighIndex, highNameLowIndex]) == "10.0.0.9");
}

@("selectHostIp breaks ifIndex ties on the lexicographically smallest name") unittest {
    IpCandidate wlan;
    wlan.ifIndex = 4;
    wlan.ifName = "wlan1";
    wlan.v4 = [10, 0, 0, 5];
    wlan.hasV4 = true;

    IpCandidate eth;
    eth.ifIndex = 4;
    eth.ifName = "eth1";
    eth.v4 = [10, 0, 0, 3];
    eth.hasV4 = true;

    assert(selectHostIp([wlan, eth]) == "10.0.0.3");
    assert(selectHostIp([eth, wlan]) == "10.0.0.3");
}

@("selectHostIp skips non-reportable candidates and falls back to unknown") unittest {
    IpCandidate loopback;
    loopback.ifIndex = 1;
    loopback.ifName = "lo";
    loopback.v4 = [127, 0, 0, 1];
    loopback.hasV4 = true;

    IpCandidate linkLocal;
    linkLocal.ifIndex = 2;
    linkLocal.ifName = "eth0";
    linkLocal.v4 = [169, 254, 9, 9];
    linkLocal.hasV4 = true;

    IpCandidate global_;
    global_.ifIndex = 3;
    global_.ifName = "eth1";
    global_.v4 = [192, 168, 1, 10];
    global_.hasV4 = true;

    assert(selectHostIp([loopback, linkLocal, global_]) == "192.168.1.10");
    assert(selectHostIp([loopback, linkLocal]) == "unknown");
    assert(selectHostIp([]) == "unknown");
}

@("startupMessage assembles the exact record payload") unittest {
    import std.algorithm.searching : canFind;
    import std.json : parseJSON;

    auto payload = startupMessage("2026-11-12T18:11:21Z", "box", "192.168.1.10", "joe");
    assert(!payload.canFind('\n'), "the syslog MSG cannot carry a newline");

    auto j = parseJSON(payload);
    assert(j["timestamp"].str == "2026-11-12T18:11:21Z");
    assert(j["host"].str == "box");
    assert(j["ip"].str == "192.168.1.10");
    assert(j["userId"].str == "joe");
    assert(j["applicationName"].str == ProgramName);
    assert(j["version"].str == appVersion);
    assert(j["eventName"].str == startupEventName);
    // A release bump must consciously update the pin in llm.config.
    assert(appVersion == "1.0.0");
}

@("startupMessage stays single-line when the identity data carries newlines") unittest {
    import std.algorithm.searching : canFind;
    import std.json : parseJSON;

    auto payload = startupMessage("t", "a\nb", "1.2.3.4", "u\nv");
    assert(!payload.canFind('\n'), "the syslog MSG cannot carry a newline");

    auto j = parseJSON(payload);
    assert(j["host"].str == "a\nb");
    assert(j["userId"].str == "u\nv");
}

@("startupMessage escapes JSON metacharacters in the identity data") unittest {
    import std.json : parseJSON;

    auto j = parseJSON(startupMessage("t", "bo\"x", "1.2.3.4", "jo\\e"));
    assert(j["host"].str == "bo\"x");
    assert(j["userId"].str == "jo\\e");
}

@("hostIp, hostName and userName never throw and never return an empty string") unittest {
    assert(!hostIp().empty);
    assert(!hostName().empty);
    assert(!userName().empty);
}
