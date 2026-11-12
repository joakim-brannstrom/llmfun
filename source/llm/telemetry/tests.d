/// Unit tests for the syslog telemetry ship. These live in a sibling
/// module rather than llm.telemetry itself because the generated dub test
/// root imports plain .d modules but not package.d, so a unittest inside
/// package.d would never run under the nusilly runner.
module llm.telemetry.tests;

import my.actor : makeSystem;

import llm.config : SyslogTelemetryConfig;
import llm.telemetry : sendSyslog;

@("the default config ships the startup record through the local syslog")
unittest {
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown;

    sendSyslog(&sys, SyslogTelemetryConfig());
}

@("a configured remote address ships the startup record over TCP")
unittest {
    import core.time : MonoTime, dur;
    import std.algorithm.searching : canFind, endsWith, startsWith;
    import std.socket : AddressFamily, InternetAddress, Socket, SocketSet, SocketType;

    auto listener = new Socket(AddressFamily.INET, SocketType.STREAM);
    listener.bind(new InternetAddress("127.0.0.1", 0));
    listener.listen(1);

    auto sys = makeSystem;
    scope (exit) {
        sys.shutdown;
        listener.close;
    }

    auto cfg = SyslogTelemetryConfig();
    cfg.address = "127.0.0.1";
    cfg.port = (cast(InternetAddress) listener.localAddress).port;
    sendSyslog(&sys, cfg); // returns before the background ship completes

    // The ship runs on the pool: a bounded wait for the LF-terminated
    // record on the accepted connection (read to EOF).
    string received;
    const deadline = MonoTime.currTime + 5.dur!"seconds";
    while (received.length == 0 && MonoTime.currTime < deadline) {
        auto readSet = new SocketSet;
        readSet.add(listener);
        if (Socket.select(readSet, null, null, 250.dur!"msecs") <= 0)
            continue;
        auto conn = listener.accept;
        char[1024] buf;
        for (auto n = conn.receive(buf); n > 0; n = conn.receive(buf))
            received ~= buf[0 .. n];
        conn.close;
    }

    assert(received.length > 0, "no startup record reached the collector");
    // facility user | severity notice composes PRI 13; the payload is the
    // CEE-tagged JSON. host, ip and user vary by machine, so the asserts
    // pin only the stable keys.
    assert(received.startsWith("<13>1 - - - - - - @cee:{"), received);
    assert(received.canFind(`"eventName":"startup"`), received);
    assert(received.canFind(`"applicationName":"llmfun"`), received);
    assert(received.canFind(`"version":"1.0.0"`), received);
    assert(received.endsWith("}\n"), received);
}

@("a remote ship failure never reaches the caller")
unittest {
    auto sys = makeSystem;
    scope (exit)
        sys.shutdown;

    auto cfg = SyslogTelemetryConfig();
    cfg.address = "127.0.0.1";
    cfg.port = 1; // nothing listens here; the connect fails inside the actor
    sendSyslog(&sys, cfg); // no exception: the failure is traced and dropped
}
