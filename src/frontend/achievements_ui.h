#pragma once

#include <SDL.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "achievements.h"
#include "settings.h"

namespace plumnin {

// The RetroAchievements part of the interface: the Logros menu, the account window, the
// list of the game's achievements and what is drawn over the game (the achievement just
// earned, challenges, progress and leaderboard counters), which is seen in full screen
// and with the menu bar hidden too. The badges are downloaded once and kept as textures.
class AchievementsUi {
public:
    AchievementsUi(Settings& settings, Achievements& achievements);
    ~AchievementsUi();
    AchievementsUi(const AchievementsUi&) = delete;
    AchievementsUi& operator=(const AchievementsUi&) = delete;

    // The Logros menu's items. Returns true when something in Settings changed (being
    // on, hardcore): main() applies it and saves.
    bool drawMenu(bool romLoaded);
    // The account and list windows; the same return as drawMenu().
    bool drawWindows();
    // Over the game, below the menu bar (`top` pixels down). `gameHidden` while a window
    // covers the paused game (the library, the states, the file browser): its indicators
    // belong to the game and wait for it, the notices are still shown.
    void drawOverlay(float top, bool gameHidden);
    void openAccount() { showAccount_ = true; }
    bool windowOpen() const { return showAccount_ || showList_; }

    // Something to show for a while: an achievement earned, the game identified...
    void show(const Achievements::Notice& notice);

private:
    struct Badge;
    struct Downloads;
    // The badge at `url` as a texture, 0 while it is downloaded (or if it cannot be).
    unsigned int badge(const std::string& url);
    void receiveBadges();
    void drawAccount(bool& changed);
    void drawList();

    Settings& settings_;
    Achievements& ra_;
    bool showAccount_ = false;
    bool showList_ = false;
    char user_[64] = "";
    char password_[128] = "";
    std::map<std::string, std::unique_ptr<Badge>> badges_;
    std::shared_ptr<Downloads> downloads_;
    struct Popup {
        Achievements::Notice notice;
        Uint32 until = 0;
    };
    std::deque<Popup> popups_;
};

}  // namespace plumnin
