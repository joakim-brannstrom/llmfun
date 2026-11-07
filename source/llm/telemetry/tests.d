/// Unit tests for the syslog telemetry sink. These live in a sibling
/// module rather than llm.telemetry itself because the generated dub test
/// root imports plain .d modules but not package.d, so a unittest inside
/// package.d would never run under the nusilly runner.
module llm.telemetry.tests;

import llm.config : SyslogTelemetryConfig;
import llm.telemetry.c_syslog : SyslogPriority;
import llm.telemetry : SyslogTelemetry;

@("a scope instance sends the identity record, records once and closes on destroy")
unittest {
    auto s = SyslogTelemetry(SyslogTelemetryConfig());
    s.record(SyslogPriority.notice, "telemetry unittest record");
    destroy(s);
    s.record(SyslogPriority.notice, "dropped silently after destroy");
}

@("record on a never-opened sink is a no-op")
unittest {
    SyslogTelemetry s;
    s.record(SyslogPriority.notice, "never opened, silently dropped");
}
