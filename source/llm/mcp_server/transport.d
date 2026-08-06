/// Stdio transport for MCP JSON-RPC over newline-delimited stdio.
/// Uses unbuffered POSIX reads from the fd to avoid the classic poll/FILE
/// buffering race: when the C FILE buffer eagerly reads multiple lines,
/// poll(2) sees an empty kernel buffer and returns 0, causing the actor
/// to stall on already-available data. Bypassing FILE fixes this and also
/// means readMessage() never blocks in the actor loop (the actor only
/// calls it after hasData() confirms a complete line is available).
module llm.mcp_server.transport;

import std.stdio : stdout;
import std.string : indexOf, stripRight;

import core.sys.posix.fcntl : F_GETFL, F_SETFL, O_NONBLOCK, fcntl;
import core.sys.posix.poll : POLLIN, POLLHUP, POLLERR, poll, pollfd;
import core.sys.posix.unistd : read;

interface Transport {
    /// Read the next JSON message string from the transport.
    /// Skips blank lines. Throws `EOFException` on end of stream.
    string readMessage();

    /// Write a JSON message string to the transport.
    /// Write errors propagate to the caller.
    void writeMessage(string jsonMessage);

    /// Close the transport. Idempotent -- subsequent calls are no-ops.
    void close();

    /// Whether the transport has been closed.
    bool closed();

    /// Whether a message is immediately available to read without blocking.
    /// Used by the actor event loop to poll the transport for input while
    /// waiting for control messages.
    bool hasData();
}

/// Thrown when the transport reaches end of input stream.
class EOFException : Exception {
    this(string file = __FILE__, size_t line = __LINE__) pure @safe {
        super("End of input stream", file, line);
    }
}

/// Transport that reads JSON messages from stdin and writes to stdout.
/// Uses direct fd reads with a pending buffer so that poll-like
/// availability checks are always accurate -- no hidden FILE buffer.
class StdioTransport : Transport {
private:
    bool _closed = false;
    int _fd;
    char[] _buf; // accumulated raw bytes from the fd

    enum Kibi = 1024;
    enum ReadChunkSize = 4 * Kibi; // 4096; typical MCP message fits in one chunk

    /// Whether _buf contains a complete line (terminated by \n).
    bool _hasCompleteLine() @safe {
        return indexOf(_buf, '\n') >= 0;
    }

    /// Non-blocking read from _fd into _buf. Returns true if new data
    /// was appended, false on EOF, EAGAIN, or error.
    /// Logs unexpected errors and marks the transport closed so the
    /// caller does not spin on a broken fd.
    bool _tryRead() @system {
        char[ReadChunkSize] tmp;
        auto n = read(_fd, tmp.ptr, tmp.length);
        if (n > 0) {
            _buf ~= tmp[0 .. n];
            return true;
        } else if (n == 0) {
            _closed = true;
        } else {
            // n < 0: EAGAIN / EWOULDBLOCK and EINTR are normal for a
            // non-blocking fd. Anything else is a real error.
            import core.stdc.errno : EAGAIN, EINTR, errno;

            if (errno != EAGAIN && errno != EINTR) {
                import logger = std.logger;

                logger.warning("Unexpected read error on stdin fd: errno=", errno);
                _closed = true;
            }
        }
        return false;
    }

    /// Extract the next complete line from _buf, stripping the trailing
    /// newline and an optional carriage return. The caller guarantees
    /// _hasCompleteLine() is true.
    string _extractLine() {
        auto nl = indexOf(_buf, '\n');
        auto raw = _buf[0 .. nl];
        if (raw.length > 0 && raw[$ - 1] == '\r')
            raw = raw[0 .. $ - 1];
        _buf = _buf[nl + 1 .. $];
        return raw.idup;
    }

public:
    this() @system {
        _fd = 0; // stdin fileno
        // Make the fd non-blocking so _tryRead() never stalls the actor.
        auto flags = fcntl(_fd, F_GETFL);
        fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
    }

    bool closed() @safe {
        return _closed;
    }

    /// Fast, non-blocking check: returns true when a complete line is
    /// buffered or can be read from the fd without blocking. Loops
    /// _tryRead() until EAGAIN so that large messages (> ReadChunkSize)
    /// are fully drained in one pass instead of incurring a 10 ms
    /// receiveTimeout cycle between chunks.

    bool hasData() @system {
        // After draining, _tryRead may have set _closed on error; partial data
        // (no \n) is discarded — MCP messages are always newline-terminated.
        if (_hasCompleteLine())
            return true;
        if (_closed)
            return false;
        // Drain all available kernel data without blocking.
        while (_tryRead()) {
            if (_hasCompleteLine())
                return true;
        }
        return _hasCompleteLine();
    }

    /// Read the next non-blank JSON line. In the actor loop this is only
    /// called after hasData() confirms availability, so it extracts
    /// immediately. When called standalone (e.g. tests) it does a
    /// short poll-and-read until a line arrives or EOF occurs.
    string readMessage() @system {
        for (;;) {
            if (_hasCompleteLine()) {
                auto msg = _extractLine();
                auto trimmed = msg.stripRight();
                if (trimmed.length > 0)
                    return trimmed;
                // Blank line -- drain any further kernel data before
                // looping so we don't fall into the poll wait below.
                while (_tryRead()) {
                }
                continue;
            }
            if (_closed)
                throw new EOFException();

            // No complete line yet. In the actor loop this path is never
            // reached (hasData() is called first). For standalone callers
            // we wait briefly and retry so we don't busy-spin.
            pollfd[1] fds;
            fds[0].fd = _fd;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            poll(fds.ptr, 1, 100); // 100 ms timeout
            _tryRead();
        }
    }

    void writeMessage(string jsonMessage) @system {
        stdout.writeln(jsonMessage);
        stdout.flush();
    }

    void close() @system {
        _closed = true;
        stdout.flush();
        // No fd close -- the process owns stdin. Setting _closed is
        // enough: hasData() returns false, and readMessage() throws
        // EOFException on next call if the buffer is drained.
    }
}
