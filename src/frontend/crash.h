#pragma once

#include <string>

namespace plumnin {

// When plumnin dies of a crash (an access violation, an abort, an uncaught exception),
// writes a report to dir: plumnin-AAAAMMDD-HHMMSS.txt with what happened, where and with
// which game, and on Windows a minidump (.dmp) next to it to open in a debugger. Then
// it lets the system end the program as it would have.
void installCrashHandler(const std::string& dir);

// What the report says plumnin was doing: the game that is loaded ("" for none).
void setCrashContext(const std::string& game);

// The report the last run left behind and nobody has been told about yet, or "" if it
// ended well. Marks it as told.
std::string takeCrashReport(const std::string& dir);

// The version this build is (the release's own, or git describe), as About shows it.
const char* appVersion();

// The window's name with no game: "plumnin 0.1.0-beta.16".
std::string appTitle();

}  // namespace plumnin
