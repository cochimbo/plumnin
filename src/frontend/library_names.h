#pragma once

#include <string>
#include <vector>

namespace plumnin {

// Names for the game library, worked out from ROM file names in the No-Intro style
// ("Legend of Zelda, The (USA).nes"). No SDL here, so the tests can use it.

// The name to show: tags in brackets dropped and a trailing ", The" moved to the front
// ("The Lion King").
std::string gameTitle(const std::string& fileStem);
// The tags in brackets, e.g. "USA, Europe · Rev A"; empty if there are none.
std::string gameTags(const std::string& fileStem);
// For searching: lower case letters and digits only, words separated by one space.
std::string searchKey(const std::string& text);

// File names (without ".png") to try in the libretro-thumbnails boxart sets, best
// first: the whole name, then with fewer trailing tags, then the bare title with the
// usual regions. Characters the set cannot use in file names (&*/:`<>?\|") become '_'.
std::vector<std::string> coverNames(const std::string& fileStem);
// The box art names (without ".png") in a listing of a Named_Boxarts folder: either
// the HTML index of thumbnails.libretro.com (href="...png", percent-encoded) or the
// JSON of GitHub's git trees API ("path": "...png"). Sorted, without repeats.
std::vector<std::string> parseCoverList(const std::string& listing);
// The same for any listing and file extension (".cht" for the cheat lists).
std::vector<std::string> parseFileList(const std::string& listing, const std::string& extension);
// The cheat files (names without ".cht") of a libretro-database cht folder, `index`
// as sorted by parseFileList(), to offer for the ROM `fileStem`: the ones named like
// coverNames() would try, each followed by its "(Action Replay)" list, then the `max`
// similarCoverNames() ("(Rumbles)" ones last). Best first, without repeats.
std::vector<std::string> cheatFileNames(const std::vector<std::string>& index, const std::string& fileStem, size_t max);
// The `max` names of `names` that look most like `query` (a game's name as the user
// typed it), best first. Words are compared without case or punctuation, a word may
// be the start of another or have a typo, and roman numerals match digits ("II" is
// "2"). Names that share nothing with the query are left out.
std::vector<std::string> similarCoverNames(const std::vector<std::string>& names, const std::string& query, size_t max);
// Percent-encodes everything but letters, digits and -._~ (spaces become %20).
std::string urlEncode(const std::string& text);

}  // namespace plumnin
