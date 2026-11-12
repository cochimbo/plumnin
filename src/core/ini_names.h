#pragma once

#include <string>

namespace plumnin {

// The name a key of the .ini files (plumnin.ini, the per-game ones in profiles/) is read
// as. plumsg's files had Spanish names up to its 0.0.31; plumnin only ever wrote English
// ones, so this is where an old name would be mapped if one is ever renamed.
inline const std::string& iniName(const std::string& name) { return name; }

}  // namespace plumnin
