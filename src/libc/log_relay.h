// Carries the game's telemetry socket over HTTP.
//
// The game opens one TCP connection per telemetry message to the log host it was given, writes the message and
// closes. connect() to LOG_PORT is answered with a local socket pair instead: a worker collects what the game writes
// and posts it to <server>/logrelay, then writes the 4-byte acknowledgement the log server sends back. <server> is the
// Java system property "a32.logrelay", set by the build that points the client at a private server. Without that
// property the connection is refused and nothing is sent.
#pragma once

#include <netinet/in.h>

#include "runtime/runtime.h"

enum : int { LOG_PORT = 4346 };

enum class LogRelay { NotLog, Relayed, Refused };

LogRelay log_relay_connect(int fd, const sockaddr_in& target);
