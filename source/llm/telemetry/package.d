module llm.telemetry;

import llm.config : SyslogFacility, SyslogTelemetryConfig;
import llm.telemetry.c_syslog : closelog, openlog, SyslogFacilityCode,
    SyslogOption, SyslogPriority, syslogStr;
import llm.telemetry.identity : hostIp, identityMessage, userName;
import std.string : toStringz;

/// The syslog telemetry sink: opens the log with the configured ident,
/// sends the one-shot identity record at LOG_NOTICE and closes the log
/// when the instance goes out of scope. Usage is a stack instance on the
/// agent main thread, so the destructor runs deterministically.
struct SyslogTelemetry {
    /// A copy would run a second destructor and close the log early.
    @disable this(this);

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
        syslogStr(SyslogPriority.notice, identityMessage(hostIp(), userName()));
    }

    ~this() @safe {
        if (isOpen) {
            closelog();
            isOpen = false;
        }
    }

    void record(SyslogPriority priority, string msg) @safe {
        if (!isOpen)
            return;
        syslogStr(priority, msg);
    }
}

// Cross-pin: the config-facing facility enum and the binding enum must
// mirror the same POSIX codes.
static assert(cast(int) SyslogFacility.user == SyslogFacilityCode.user);
static assert(cast(int) SyslogFacility.daemon == SyslogFacilityCode.daemon);
static assert(cast(int) SyslogFacility.local7 == SyslogFacilityCode.local7);
