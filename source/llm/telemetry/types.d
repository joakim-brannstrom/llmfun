/// Shared telemetry vocabulary: the collector target variants and the
/// textual-target helpers.
module llm.telemetry.types;

import std.sumtype;

/// An IPv4 collector target, e.g. addr "192.168.1.22" port 514. Kept textual
/// and wide so config parsing never wraps; narrowed to a socket Address only
/// at send time, where a port outside 0..65535 is a badTarget error.
struct IpV4 {
    string addr;
    long port;

    this(string addr, long port) pure @safe {
        this.addr = addr;
        this.port = port;
    }
}

/// ditto for IPv6, e.g. addr "::1".
struct IpV6 {
    string addr;
    long port;

    /// ditto
    this(string addr, long port) pure @safe {
        this.addr = addr;
        this.port = port;
    }
}

alias Ip = SumType!(IpV4, IpV6);

/// The target from a textual address and port: an address containing ':'
/// is IPv6 (e.g. "::1"), otherwise a dotted quad.
Ip ipTarget(string address, long port) pure @safe {
    import std.algorithm.searching : canFind;

    if (address.canFind(':'))
        return Ip(IpV6(address, port));
    return Ip(IpV4(address, port));
}

/// Human readable target for trace logs: addr:port, bracketed for IPv6
/// where the colons would otherwise be ambiguous.
string render(Ip ip) pure @safe {
    import std.conv : text;

    return ip.match!((IpV4 v4) => i"$(v4.addr):$(v4.port)".text, (IpV6 v6) => i"[$(v6.addr)]:$(
            v6.port)".text);
}

@("a textual address and port classify by family and render")
unittest {
    assert(render(ipTarget("192.168.1.22", 514)) == "192.168.1.22:514",
            "a dotted quad must classify as IPv4");
    assert(render(ipTarget("::1", 514)) == "[::1]:514",
            "an address with a colon must classify as IPv6");
}
