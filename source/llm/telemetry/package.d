module llm.telemetry;

import std.array : empty;
import std.string : toStringz;

import my.actor : System;

import llm.config : SyslogFacility, SyslogTelemetryConfig;
import llm.telemetry.c_syslog : SyslogPriority, SyslogFacilityCode,
    SyslogOption, closelog, openlog, syslogStr;
import llm.telemetry.identity : hostIp, hostName, startupMessage, userName;
import llm.telemetry.remote_syslog : RemoteSyslogActor;
import llm.telemetry.types : ipTarget;

/// The one-shot ship of the startup record: synchronously through the
/// local libc syslog, fire-and-forget through the actor pool when a
/// remote collector is configured.
void sendSyslog(System* sys, in SyslogTelemetryConfig cfg) {
    if (cfg.address.empty) {
        auto sink = new LocalSyslog(cfg);
        sink.record(SyslogPriority.notice, startupPayload());
        sink.close();
        return;
    }

    sys.spawn!RemoteSyslogActor(ipTarget(cfg.address, cfg.port),
            cast(int) cfg.facility | SyslogPriority.notice, startupPayload());
}

private string startupPayload() @safe {
    import std.datetime : Clock;

    return startupMessage(Clock.currTime.toUTC.toISOExtString(0), hostName(),
            hostIp(), userName());
}

/// The local libc syslog sink: opens the log with the configured ident at
/// construction, ships every record to it and releases the log on close.
final class LocalSyslog {
    /// Keeps the ident buffer alive for every message (libc stores the
    /// ident pointer as-is).
    private string ident;
    private bool isOpen;

    this(in SyslogTelemetryConfig cfg) @safe {
        ident = cfg.ident.idup;
        // Only pid | ndelay; perror and cons would break the no-console-output
        // rule.
        openlog(ident.toStringz, SyslogOption.pid | SyslogOption.ndelay, cast(int) cfg.facility);
        isOpen = true;
    }

    void record(SyslogPriority priority, string msg) @safe {
        if (isOpen)
            syslogStr(priority, msg);
    }

    void close() @safe {
        if (isOpen) {
            closelog();
            isOpen = false;
        }
    }

    ~this() @safe {
        close(); // safety net for GC collected instances
    }
}

// Cross-pin: the config-facing facility enum and the binding enum must
// mirror the same POSIX codes.
static assert(cast(int) SyslogFacility.user == SyslogFacilityCode.user);
static assert(cast(int) SyslogFacility.daemon == SyslogFacilityCode.daemon);
static assert(cast(int) SyslogFacility.local7 == SyslogFacilityCode.local7);
