/// Remote syslog over plain TCP with only std.socket: LF-terminated RFC
/// 5424 records with a CEE-tagged JSON MSG, shipped by a one-shot actor
/// off the caller's thread; every failure is a code, never an exception.
module llm.telemetry.remote_syslog;

import core.attribute : mustuse;

import core.time : Duration, MonoTime, dur;

import llm.telemetry.types : Ip, IpV4, IpV6, render;
import logger = std.logger;
import my.actor : ActorRef;
import std.socket;
import std.sumtype;

/// PRI of the record when the caller does not care: facility user |
/// severity info, the value the reference implementation shipped. Compose
/// other priorities as facility | severity.
enum defaultPri = 14;

/// The generous deadline for one remote ship: the connect and the send
/// together are bounded by this, so a dropping or stalled collector holds
/// the shipping actor at most this long instead of the OS connect
/// timeout's minutes.
immutable Duration sendDeadline = 10.dur!"seconds";

/// What failed in a remote syslog attempt.
enum SyslogErrorCode {
    /// The step succeeded.
    none,
    /// The target address or port does not map to a socket address.
    badTarget,
    /// The collector refused or timed out on the connection.
    connectFailed,
    /// The record did not go out in full: the connection broke or the
    /// deadline expired.
    sendFailed,
}

/// The outcome of one remote syslog step: an error code plus detail for
/// trace logging. Default init is success, so a failure is always built
/// explicitly. @mustuse makes discarding one a compile error.
@mustuse struct SyslogError {
    /// What failed; none when the step succeeded.
    SyslogErrorCode code = SyslogErrorCode.none;
    /// Detail for trace logging; empty when none.
    string msg;

    this(SyslogErrorCode code, string msg = "") pure @safe @nogc {
        this.code = code;
        this.msg = msg;
    }

    /// True when the step succeeded.
    bool ok() const pure @safe nothrow {
        return code == SyslogErrorCode.none;
    }
}

/// The record for the payload: PRI, version 1, nil TIMESTAMP, HOSTNAME,
/// APP-NAME, PROCID and MSGID, nil STRUCTURED-DATA, then the CEE tag and
/// the JSON as the free-text MSG. The CEE tag makes rsyslog and syslog-ng
/// treat the JSON as structured content; the LF is RFC 6587 non-transparent
/// framing, and the single-line JSON can never split the frame.
string syslogMessage(string jsonPayload, int pri = defaultPri) pure @safe {
    import std.conv : text;

    return i"<$(pri)>1 - - - - - - @cee:$(jsonPayload)\n".text;
}

/// The one-shot blocking shipper: build the record and send it to the
/// collector in full.
@safe SyslogError sendSyslogRecord(Ip target, string jsonPayload, int pri = defaultPri) {
    return sendOverTcp(target, syslogMessage(jsonPayload, pri));
}

/// The one-shot actor: ships one record to the collector in onSpawn, off
/// the caller's thread.
final class RemoteSyslogActor {
    /// The collector dialed for the record.
    private Ip target;
    /// Composed PRI of the record, facility | severity.
    private int pri;
    /// The JSON payload shipped as the CEE MSG.
    private string payload;

    this(Ip target, int pri, string payload) @safe {
        this.target = target;
        this.pri = pri;
        this.payload = payload;
    }

    void onSpawn(ActorRef self) @safe {
        import std.exception : collectException;

        auto err = sendSyslogRecord(target, payload, pri);
        if (!err.ok)
            logger.tracef("remote syslog to %s failed: %s", render(target),
                    err.msg).collectException;
    }
}

/// The socket Address for the target, written into addr; a failure comes
/// back through err and leaves addr null. Never throws: an unparsable
/// address or a port outside 0..65535 becomes badTarget instead of an
/// exception or a silent wrap.
@safe SyslogError toAddress(Ip ip, out Address addr) {
    import std.conv : text;

    auto port = ip.match!((IpV4 v4) => v4.port, (IpV6 v6) => v6.port);
    if (port < 0 || port > ushort.max)
        return SyslogError(SyslogErrorCode.badTarget, i"syslog port $(port) outside 0..65535".text);

    try {
        addr = ip.match!((IpV4 v4) => cast(Address) new InternetAddress(v4.addr,
                cast(ushort) v4.port), (IpV6 v6) => cast(Address) new Internet6Address(v6.addr,
                cast(ushort) v6.port));
    } catch (Exception e) {
        return SyslogError(SyslogErrorCode.badTarget, e.msg);
    }
    return SyslogError.init;
}

/// Dial, send the record in full, close
@safe SyslogError sendOverTcp(Ip target, string message, Duration deadline = sendDeadline) {
    import std.datetime : Clock;

    Address addr;
    auto err = toAddress(target, addr);
    if (!err.ok)
        return err;

    TcpSocket socket;
    try {
        socket = new TcpSocket(addr.addressFamily);
    } catch (SocketException e) {
        return SyslogError(SyslogErrorCode.connectFailed, e.msg);
    }
    scope (exit)
        socket.close;

    const start = Clock.currTime;

    // Non-blocking connect: a dropping or filtered collector must not hold
    // the ship on a blocking connect, which defaults to minutes on Linux.
    try {
        socket.blocking = false;
        socket.connect(addr);
    } catch (SocketException e) {
        return SyslogError(SyslogErrorCode.connectFailed, e.msg);
    }

    // connect reports EINPROGRESS without blocking (see std.socket);
    // writability then means the attempt finished, and SO_ERROR says
    // whether it succeeded. 0 is a deadline hit, -1 an interruption;
    // both give up the best-effort ship.
    int soError;
    try {
        auto writeSet = new SocketSet;
        auto errorSet = new SocketSet;
        writeSet.add(socket);
        errorSet.add(socket);
        if (Socket.select(null, writeSet, errorSet, deadline) <= 0)
            return SyslogError(SyslogErrorCode.connectFailed,
                    "connection attempt did not complete within the syslog deadline");
        socket.getOption(SocketOptionLevel.SOCKET, SocketOption.ERROR, soError);
    } catch (SocketException e) {
        return SyslogError(SyslogErrorCode.connectFailed, e.msg);
    }
    if (soError != 0)
        return SyslogError(SyslogErrorCode.connectFailed, formatSocketError(soError));

    // Blocking send bounded by what is left of the deadline; a 0 return
    // counts as a failure too, so a stuck socket can never spin the loop.
    const remaining = deadline - (Clock.currTime - start);
    if (remaining <= Duration.zero)
        return SyslogError(SyslogErrorCode.sendFailed, "syslog deadline expired before the send");
    try {
        socket.blocking = true;
        socket.setOption(SocketOptionLevel.SOCKET, SocketOption.SNDTIMEO, remaining);
    } catch (SocketException e) {
        return SyslogError(SyslogErrorCode.sendFailed, e.msg);
    }

    size_t sent;
    while (sent < message.length) {
        auto n = socket.send(message[sent .. $]);
        if (n <= 0)
            return SyslogError(SyslogErrorCode.sendFailed, "remote syslog send failed");
        sent += n;
    }
    return SyslogError.init;
}

@("the record wraps the payload as CEE-tagged RFC 5424 free text, LF-terminated")
unittest {
    auto payload = `{"event":"startup","status":"success","version":"1.0.0"}`;
    assert(syslogMessage(
            payload) == `<14>1 - - - - - - @cee:{"event":"startup","status":"success","version":"1.0.0"}`
            ~ "\n");
    assert(syslogMessage(`{"k":1}`, 165) == `<165>1 - - - - - - @cee:{"k":1}` ~ "\n");
}

@("toAddress maps an IPv4 target to the matching socket address")
unittest {
    Address addr;
    auto err = toAddress(Ip(IpV4("127.0.0.1", 514)), addr);
    assert(err.ok, err.msg);
    auto v4 = cast(InternetAddress) addr;
    assert(v4 !is null);
    assert(v4.toAddrString == "127.0.0.1");
    assert(v4.port == 514);
}

@("toAddress reports a port outside 0..65535 as badTarget")
unittest {
    Address addr;
    auto err = toAddress(Ip(IpV4("127.0.0.1", 99_999)), addr);
    assert(err.code == SyslogErrorCode.badTarget, err.msg);
    assert(addr is null);
}

@("sendOverTcp delivers the record to a TCP listener over IPv4")
unittest {
    auto listener = new TcpSocket(AddressFamily.INET);
    listener.bind(new InternetAddress("127.0.0.1", 0));
    listener.listen(1);
    auto bound = cast(InternetAddress) listener.localAddress;
    auto target = Ip(IpV4(bound.toAddrString(), bound.port));

    auto err = sendOverTcp(target, "hello syslog");
    assert(err.ok, err.msg);

    auto conn = listener.accept();
    scope (exit) {
        conn.close;
        listener.close;
    }

    string received;
    char[1024] buf;
    for (auto n = conn.receive(buf); n > 0; n = conn.receive(buf))
        received ~= buf[0 .. n];

    assert(received == "hello syslog", received);
}

@("sendSyslogRecord delivers the record to a TCP collector over IPv4")
unittest {
    auto payload = `{"event":"startup","status":"success"}`;
    auto listener = new TcpSocket(AddressFamily.INET);
    listener.bind(new InternetAddress("127.0.0.1", 0));
    listener.listen(1);
    auto bound = cast(InternetAddress) listener.localAddress;

    auto err = sendSyslogRecord(Ip(IpV4(bound.toAddrString(), bound.port)), payload);
    assert(err.ok, err.msg);

    auto conn = listener.accept();
    scope (exit) {
        conn.close;
        listener.close;
    }

    string received;
    char[1024] buf;
    for (auto n = conn.receive(buf); n > 0; n = conn.receive(buf))
        received ~= buf[0 .. n];

    assert(received == syslogMessage(payload), received);
}

@("sendSyslogRecord delivers the record to a TCP collector over IPv6")
unittest {
    auto payload = `{"event":"startup","status":"success"}`;
    auto listener = new TcpSocket(AddressFamily.INET6);
    listener.bind(new Internet6Address("::1", 0));
    listener.listen(1);
    auto bound = cast(Internet6Address) listener.localAddress;

    auto err = sendSyslogRecord(Ip(IpV6(bound.toAddrString(), bound.port)), payload);
    assert(err.ok, err.msg);

    auto conn = listener.accept();
    scope (exit) {
        conn.close;
        listener.close;
    }

    string received;
    char[1024] buf;
    for (auto n = conn.receive(buf); n > 0; n = conn.receive(buf))
        received ~= buf[0 .. n];

    assert(received == syslogMessage(payload), received);
}

@("sendSyslogRecord reports connectFailed when the collector refuses the connection")
unittest {
    auto held = new TcpSocket(AddressFamily.INET);
    held.bind(new InternetAddress("127.0.0.1", 0));
    held.listen(1);
    auto port = (cast(InternetAddress) held.localAddress).port;
    held.close;

    auto err = sendSyslogRecord(Ip(IpV4("127.0.0.1", port)), `{"k":1}`);
    assert(err.code == SyslogErrorCode.connectFailed, err.msg);
}
