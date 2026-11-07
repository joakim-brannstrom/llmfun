module llm.telemetry.c_syslog;

/// openlog option bits (glibc sys/syslog.h values; none = no option bits).
enum SyslogOption : int {
    none = 0,
    pid = 0x01,
    cons = 0x02,
    odelay = 0x04,
    ndelay = 0x08,
    nowait = 0x10,
    perror = 0x20,
}

/// syslog priority levels, LOG_EMERG=0 through LOG_DEBUG=7. The D keyword
/// `debug` is not a legal identifier, so the last level is `debug_` (the
/// same convention CompressionMode.auto_ uses for `auto`).
enum SyslogPriority : int {
    emerg = 0,
    alert = 1,
    crit = 2,
    err = 3,
    warning = 4,
    notice = 5,
    info = 6,
    debug_ = 7,
}

/// Raw POSIX facility codes. Only the values mirrored by llm.config's
/// SyslogFacility (user, daemon, local0..local7) are needed here.
enum SyslogFacilityCode : int {
    user = 1 << 3,
    daemon = 3 << 3,
    local0 = 16 << 3,
    local1 = 17 << 3,
    local2 = 18 << 3,
    local3 = 19 << 3,
    local4 = 20 << 3,
    local5 = 21 << 3,
    local6 = 22 << 3,
    local7 = 23 << 3,
}

static assert(SyslogOption.pid == 0x01);
static assert(SyslogOption.ndelay == 0x08);
static assert(SyslogOption.perror == 0x20);
static assert(SyslogPriority.notice == 5);
static assert(SyslogPriority.debug_ == 7);
static assert(SyslogFacilityCode.user == 8);
static assert(SyslogFacilityCode.daemon == 24);
static assert(SyslogFacilityCode.local7 == 184);

// Silence guarantee: only SyslogOption.pid | SyslogOption.ndelay may ever be
// passed to openlog. SyslogOption.perror copies every message to stderr and
// SyslogOption.cons writes to /dev/console on send errors, both of which
// break the no-console-output rule. syslog() is a void API and cannot
// report errors.

// Attributes are per declaration: a trailing attribute block would leak
// @nogc/nothrow into a trailing unittest and break the test runner.
// syslog is @system because a C-vararg declaration cannot type-check its
// payloads: a @safe caller could pass a D dynamic array where libc expects
// a NUL-terminated pointer.
extern (C) @nogc nothrow @safe void openlog(const(char)* ident, int logOption, int facility);
extern (C) @nogc nothrow @system void syslog(int priority, const(char)* message, ...);
extern (C) @nogc nothrow @safe void closelog();
extern (C) @nogc nothrow @safe int setlogmask(int mask);

/// The sole sanctioned route to the vararg syslog: a runtime string is
/// converted to a null-terminated pointer and the format stays the single
/// `%s` literal, so a D dynamic array can never be handed to libc as a
/// vararg and user data can never land in the format position.
@trusted void syslogStr(int priority, const(char)[] msg) nothrow {
    import std.string : toStringz;

    syslog(priority, "%s", msg.toStringz);
}

@("openlog/syslog/closelog link and run without console output") unittest {
    import std.string : toStringz;

    openlog("llmfun_test".toStringz, SyslogOption.pid | SyslogOption.ndelay,
            SyslogFacilityCode.user);
    syslog(SyslogPriority.notice, "%s", "probe".toStringz);
    syslogStr(SyslogPriority.info, "probe via the fixed-format wrapper");
    closelog();
}
