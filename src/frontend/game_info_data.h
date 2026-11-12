#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace plumnin {

// The data behind the library's detail panel, read from what libretro-database and
// Wikipedia answer. No SDL here, so the tests can use it.

// What libretro-database knows of one game.
struct GameDetails {
    std::string developer, publisher, genre, year, players;
};

// The games of one console in libretro-database's metadat files, by their No-Intro
// name ("Sonic The Hedgehog 2 (World)"), and a lookup for a ROM's file name.
class DetailsTable {
public:
    // Adds one metadat file: `field` is its folder ("developer", "publisher", "genre",
    // "releaseyear" or "maxusers") and `text` its clrmamepro contents.
    void add(const std::string& field, const std::string& text);
    // The game named like the ROM `fileStem`: the same name, else the first game with
    // the same title and other tags ("(USA)" for a ROM tagged "(Europe)"). Null if none.
    const GameDetails* find(const std::string& fileStem) const;
    bool empty() const { return games_.empty(); }

private:
    std::unordered_map<std::string, GameDetails> games_;      // by lower-case name
    std::unordered_map<std::string, std::string> byTitle_;    // searchKey(title) -> name
};

// The game entries of a clrmamepro metadat file: each game's comment (or name) and the
// value of `field` ("users 2" or "genre \"Platform\""). Entries without it are left out.
std::vector<std::pair<std::string, std::string>> parseMetadat(const std::string& text, const std::string& field);

// One page of a Wikipedia search, as the MediaWiki API answers it.
struct WikiPage {
    std::string title;
    std::string extract;  // its introduction, as plain text
    int index = 0;        // its place in the search results, from 1
};
// The pages of an API answer with generator=search, prop=extracts and
// formatversion=2, in search order.
std::vector<WikiPage> parseWikiSearch(const std::string& json);
// How far the Wikipedia page `pageTitle` is from being about the game `gameTitle`: 0 for
// the same words (a "(video game)" note, case, punctuation, articles and roman numerals
// aside), else how many words only one of them has; -1 if it is not about it: each
// title has words the other lacks, they share less than half of the game's words, or
// a number is in one and not the other ("Sonic 2" is not "Sonic").
int wikiTitleDistance(const std::string& gameTitle, const std::string& pageTitle);

// libretro-database's English name of a genre as plumnin's own text, in Spanish (to be
// translated with tr()); null for the ones it does not know.
const char* genreName(const std::string& genre);

}  // namespace plumnin
