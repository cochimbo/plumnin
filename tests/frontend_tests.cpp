// Frontend tests that need SDL's key names but no window: a game's own options and
// controls, read from and written to its file in the profiles folder.
#define SDL_MAIN_HANDLED
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#include "frontend/crash.h"
#include "frontend/game_options.h"
#include "frontend/handheld.h"

#ifndef _WIN32
#include <csignal>
#include <fstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace plumnin;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr int kA = static_cast<int>(PadButton::A);
constexpr int kB = static_cast<int>(PadButton::B);
constexpr int kStart = static_cast<int>(PadButton::Start);

void testGameControls() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();

    // [keyboard] shares fast_forward with [speed]: a hotkey there is not the option.
    const GameOverrides o = parseGameOverrides(
        "[video]\nfilter = crt\n[keyboard]\na = Z\nfast_forward = F1\n[gamepad]\nb = y\n", general, generalInput);
    check(o.has(kGameControls) && o.has("filter") && o.own.size() == 2, "juego: botones propios y filtro");
    check(!o.has("fast_forward"), "juego: un atajo de [keyboard] no es la opción fast_forward");
    check(o.controls.keys[kA] == std::vector<SDL_Scancode>{SDL_SCANCODE_Z}, "juego: A en la tecla Z");
    check(o.controls.pad[kB] == std::vector<SDL_GameControllerButton>{SDL_CONTROLLER_BUTTON_Y}, "juego: B en el botón Y");
    check(o.controls.keys[kStart] == generalInput.keys[kStart], "juego: los botones que no tiene siguen a los generales");

    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.nes"), general, generalInput);
    check(back.own == o.own && back.controls.keys == o.controls.keys && back.controls.keys2 == o.controls.keys2 &&
              back.controls.pad == o.controls.pad,
          "juego: los botones propios se guardan y se leen igual");

    const GameOverrides plain = parseGameOverrides("[speed]\nfast_forward = 6\n", general, generalInput);
    check(plain.has("fast_forward") && !plain.has(kGameControls), "juego: sin [keyboard] ni [gamepad] no hay botones propios");
    check(formatGameOverrides(plain, "juego.nes").find("[keyboard]") == std::string::npos,
          "juego: sin botones propios no se escribe [keyboard]");
}

void testRunAheadOption() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();
    check(general.runAhead == 0, "run-ahead: desactivado si no se dice");

    // Its own value for a game, and back from its file.
    const GameOverrides o = parseGameOverrides("[speed]\nrun_ahead = 1\n", general, generalInput);
    check(o.has("run_ahead") && o.values.runAhead == 1, "run-ahead: un juego con su propio valor");
    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.nes"), general, generalInput);
    check(back.has("run_ahead") && back.values.runAhead == 1, "run-ahead: se guarda y se lee igual en el juego");

    // plumnin.ini keeps it, within 0 and kMaxRunAhead.
    Settings s;
    std::istringstream in("[speed]\nrun_ahead = 9\n");
    Settings::parse(in, s);
    check(s.runAhead == Settings::kMaxRunAhead, "run-ahead: no pasa del máximo");
    s.runAhead = 2;
    std::ostringstream out;
    s.write(out);
    Settings again;
    std::istringstream text(out.str());
    Settings::parse(text, again);
    check(again.runAhead == 2, "run-ahead: plumnin.ini lo guarda");
}

void testSpriteLimitOption() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();
    check(!general.unlimitedSprites, "sin límite de sprites: desactivado si no se dice");
    const GameOverrides o = parseGameOverrides("[video]\nno_sprite_limit = yes\n", general, generalInput);
    check(o.has("no_sprite_limit") && o.values.unlimitedSprites, "sin límite de sprites: un juego con su propio valor");
    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.nes"), general, generalInput);
    check(back.has("no_sprite_limit") && back.values.unlimitedSprites,
          "sin límite de sprites: se guarda y se lee igual en el juego");
    Settings s;
    s.unlimitedSprites = true;
    std::ostringstream out;
    s.write(out);
    Settings again;
    std::istringstream text(out.str());
    Settings::parse(text, again);
    check(again.unlimitedSprites, "sin límite de sprites: plumnin.ini lo guarda");
}

void testOverclockOption() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();
    check(general.overclock == 100, "overclock: desactivado si no se dice");
    const GameOverrides o = parseGameOverrides("[console]\noverclock = 200\n", general, generalInput);
    check(o.has("overclock") && o.values.overclock == 200, "overclock: un juego con su propio valor");
    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.nes"), general, generalInput);
    check(back.has("overclock") && back.values.overclock == 200, "overclock: se guarda y se lee igual en el juego");
    Settings s;
    s.overclock = 300;
    std::ostringstream out;
    s.write(out);
    Settings again;
    std::istringstream text(out.str());
    Settings::parse(text, again);
    check(again.overclock == 300, "overclock: plumnin.ini lo guarda");
    std::istringstream big("[console]\noverclock = 5000\n");
    Settings::parse(big, again);
    check(again.overclock == Settings::kMaxOverclock, "overclock: como mucho 400");
}

void testWidescreenOption() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();
    check(general.wideLeft == 0 && general.wideRight == 0, "pantalla ancha: desactivada si no se dice");
    const GameOverrides o = parseGameOverrides("[video]\nwidescreen_left = 53\nwidescreen_right = 16\n", general,
                                               generalInput);
    check(o.has("widescreen") && o.values.wideLeft == 53 && o.values.wideRight == 16,
          "pantalla ancha: un juego con sus propios lados");
    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.nes"), general, generalInput);
    check(back.has("widescreen") && back.values.wideLeft == 53 && back.values.wideRight == 16,
          "pantalla ancha: se guarda y se lee igual en el juego");
    const GameOverrides preset = parseGameOverrides("[video]\nwidescreen = 16:9\n", general, generalInput);
    check(preset.has("widescreen") && preset.values.wideLeft == 43 && preset.values.wideRight == 43,
          "pantalla ancha: lee 16:9 como el nombre de sus columnas");
    Settings s;
    s.wideLeft = 32;
    s.wideRight = 56;
    std::ostringstream out;
    s.write(out);
    Settings again;
    std::istringstream text(out.str());
    Settings::parse(text, again);
    check(again.wideLeft == 32 && again.wideRight == 56, "pantalla ancha: plumnin.ini lo guarda");
}

// Console mode and the interface's size are kept, and the handhelds are told apart by
// their firmware's model names.
void testConsoleMode() {
    Settings s;
    s.consoleMode = 1;
    s.uiScale = 150;
    std::ostringstream out;
    s.write(out);
    std::istringstream in(out.str());
    Settings back;
    Settings::parse(in, back);
    check(back.consoleMode == 1 && back.uiScale == 150, "ini: modo consola y tamaño de la interfaz");
    std::istringstream automatic("[interface]\nconsole_mode = auto\nsize = auto\n");
    Settings a;
    a.consoleMode = 2;
    a.uiScale = 120;
    Settings::parse(automatic, a);
    check(a.consoleMode == 0 && a.uiScale == 0, "ini: modo consola y tamaño automáticos");
    check(handheldFromModel("Jupiter Valve") == "Steam Deck" && handheldFromModel("Galileo Valve") == "Steam Deck" &&
              handheldFromModel("ROG Ally RC71L_RC71L ROG Ally ASUSTeK COMPUTER INC.") == "ROG Ally" &&
              handheldFromModel("Claw A1M Claw Micro-Star International Co., Ltd.") == "MSI Claw" &&
              handheldFromModel("83E1 Legion Go 8APU1 LENOVO") == "Legion Go" &&
              handheldFromModel("System Product Name To be filled by O.E.M.").empty() &&
              handheldFromModel("ROG STRIX B550-F GAMING").empty(),
          "modo consola: reconoce las portátiles por su modelo");
}

#ifndef _WIN32
// A child process crashes, in each way the handler catches; the report it leaves says
// what happened and with which game, and the next start is told about it once.
void testCrashReport() {
    const std::string dir = "frontend_tests_fallos/";
    struct Case {
        const char* name;
        void (*crash)();
        const char* says;
    };
    const Case cases[] = {
        {"acceso a memoria", [] { *static_cast<volatile int*>(nullptr) = 1; }, "SIGSEGV"},
        {"excepción sin capturar", [] { throw std::runtime_error("algo salió mal"); }, "algo salió mal"},
        {"abort", [] { std::abort(); }, "SIGABRT"},
    };
    for (const Case& c : cases) {
        const pid_t child = fork();
        if (child == 0) {
            installCrashHandler(dir);
            setCrashContext("roms/juego.nes");
            c.crash();
            _exit(0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        check(WIFSIGNALED(status), (std::string("fallo: ") + c.name + " acaba el programa como sin el informe").c_str());
        const std::string report = takeCrashReport(dir);
        std::ifstream in(report);
        std::stringstream text;
        text << in.rdbuf();
        check(!report.empty() && text.str().find(c.says) != std::string::npos &&
                  text.str().find("roms/juego.nes") != std::string::npos,
              (std::string("fallo: ") + c.name + " deja un informe con lo que pasó y el juego").c_str());
        check(takeCrashReport(dir).empty(), (std::string("fallo: ") + c.name + " se avisa una sola vez").c_str());
    }
}
#endif

}  // namespace

int main() {
    testGameControls();
    testRunAheadOption();
    testSpriteLimitOption();
    testWidescreenOption();
    testOverclockOption();

    testConsoleMode();
#ifndef _WIN32
    testCrashReport();
#endif
    std::printf(failures ? "%d fallos\n" : "todo bien\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
