#include "ui.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "gfx.h"
#include "log.h"
#include "settings.h"

// ------------------------------------------------------------------ thème

static const GfxColor BG = RGB(24, 27, 34);
static const GfxColor PANEL = RGB(36, 40, 50);
static const GfxColor PANEL_HI = RGB(50, 56, 70);
static const GfxColor TEXT = RGB(236, 238, 241);
static const GfxColor DIM = RGB(150, 156, 168);
static const GfxColor ACCENT = RGB(74, 163, 255);
static const GfxColor WOOD = RGB(226, 186, 116);
static const GfxColor DANGER = RGB(235, 90, 80);

static const int BOARD_SIDE = 704;
static const int BOARD_X = 8, BOARD_Y = 8;
static const int PANEL_X = 736, PANEL_W = 528;

struct Rect {
    int x, y, w, h;
    bool tapped(const Input& in) const { return in.tapIn(x, y, w, h); }
};

static u64 nowMs()
{
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

// Pastille de bouton manette : "A", "B", "X", "Y", "L", "R", "+", "-", "ZL", "ZR".
static int drawGlyph(int x, int y, const char* label, int size = 26)
{
    int w = std::max(size, gfx_text_width(size * 6 / 10, label) + size / 2);
    gfx_round_rect(x, y, w, size, size / 2, RGB(220, 224, 232));
    int tw = gfx_text_width(size * 6 / 10, label);
    gfx_text(x + (w - tw) / 2, y + size / 5, size * 6 / 10, RGB(30, 33, 40), label);
    return w;
}

// Barre d'aide en bas de l'écran : paires (bouton, action).
static void drawHints(const std::vector<std::pair<const char*, std::string>>& hints, int y = 684)
{
    int total = 0;
    for (auto& h : hints)
        total += std::max(26, gfx_text_width(15, h.first) + 13) + 8 + gfx_text_width(20, h.second.c_str()) + 28;
    int x = SCREEN_W - 24 - total;
    for (auto& h : hints) {
        x += drawGlyph(x, y, h.first) + 8;
        x += gfx_text(x, y + 1, 20, DIM, h.second.c_str()) + 28;
    }
}

static void drawButton(const Rect& r, const std::string& label, const char* glyph, bool focused, bool enabled = true,
                       bool active = false)
{
    GfxColor bg = active ? RGB(40, 92, 150) : (focused ? PANEL_HI : PANEL);
    gfx_round_rect(r.x, r.y, r.w, r.h, 14, bg);
    if (focused)
        gfx_round_rect(r.x, r.y, 6, r.h, 3, ACCENT);
    GfxColor tc = enabled ? TEXT : RGB(95, 100, 112);
    int fs = r.h >= 60 ? 26 : 22;
    int tx = r.x + 22;
    if (glyph) {
        tx += drawGlyph(r.x + 18, r.y + (r.h - 26) / 2, glyph) + 12;
    }
    gfx_text_fit(tx, r.y + (r.h - fs) / 2 - 2, fs, tc, label.c_str(), r.x + r.w - tx - 12);
}

static void drawTitle(const char* title, const char* subtitle = nullptr)
{
    gfx_text(64, 40, 40, TEXT, title);
    if (subtitle)
        gfx_text(66, 92, 22, DIM, subtitle);
}

// Ligne "réglage" : libellé à gauche, valeur entre deux flèches à droite.
struct ValueRow {
    Rect row, left, right;
};

static ValueRow valueRowRect(int index, int top = 150)
{
    ValueRow r;
    r.row = {120, top + index * 74, 1040, 62};
    r.left = {620, r.row.y + 4, 70, 54};
    r.right = {1084, r.row.y + 4, 70, 54};
    return r;
}

static void drawValueRow(const ValueRow& r, const char* label, const std::string& value, bool focused, bool enabled = true)
{
    gfx_round_rect(r.row.x, r.row.y, r.row.w, r.row.h, 14, focused ? PANEL_HI : PANEL);
    if (focused)
        gfx_round_rect(r.row.x, r.row.y, 6, r.row.h, 3, ACCENT);
    gfx_text(r.row.x + 28, r.row.y + 16, 26, enabled ? TEXT : DIM, label);
    GfxColor ac = enabled ? (focused ? ACCENT : DIM) : RGB(80, 84, 96);
    int cy = r.row.y + r.row.h / 2;
    gfx_triangle(r.left.x + 44, cy - 13, r.left.x + 44, cy + 13, r.left.x + 24, cy, ac);
    gfx_triangle(r.right.x + 26, cy - 13, r.right.x + 26, cy + 13, r.right.x + 46, cy, ac);
    int center = (r.left.x + r.left.w + r.right.x) / 2;
    int w = gfx_text_width(26, value.c_str());
    gfx_text(center - w / 2, r.row.y + 16, 26, enabled ? TEXT : DIM, value.c_str());
}

// -1 / +1 si la valeur de cette ligne doit changer (flèches touchées ou gauche/droite).
static int valueRowDelta(const Input& in, const ValueRow& r, bool focused)
{
    if (r.left.tapped(in))
        return -1;
    if (r.right.tapped(in))
        return 1;
    if (focused && in.repeated(HidNpadButton_AnyLeft))
        return -1;
    if (focused && in.repeated(HidNpadButton_AnyRight))
        return 1;
    return 0;
}

static const char* levelName(int level)
{
    if (level <= 2)
        return L("Beginner", "Débutant");
    if (level <= 4)
        return L("Novice", "Novice");
    if (level <= 6)
        return L("Intermediate", "Intermédiaire");
    if (level <= 8)
        return L("Advanced", "Confirmé");
    return L("Strong", "Fort");
}

// ------------------------------------------------------------------ app

App::App(Engine& engine) : m_engine(engine)
{
    // Petite position de démonstration pour le menu
    GameSetup demo;
    demo.size = 9;
    demo.black = demo.white = PlayerKind::Human;
    m_demo.start(demo);
    const char* moves[] = {"E5", "C4", "G4", "D6", "F7", "C7", "D3", "C3", "F3", "G6"};
    for (const char* m : moves)
        m_demo.play(Location::ofString(m, m_demo.board()));
}

void App::toast(const std::string& msg)
{
    m_toast = msg;
    m_toastUntil = nowMs() + 2500;
    m_dirty = true;
}

bool App::frame()
{
    const Input& in = input_update();
    if (in.down || in.touching || in.released)
        m_dirty = true;

    if (m_wasLoading && !m_engine.loading()) {
        m_wasLoading = false;
        m_engine.setAnalysisNet((NetId)g_settings.analysisNet);
        m_engine.setAnalysisMaxVisits(g_settings.analysisVisits);
        m_dirty = true;
    }

    switch (m_screen) {
    case Screen::Menu: updateMenu(in); break;
    case Screen::Setup: updateSetup(in); break;
    case Screen::Game: updateGame(in); break;
    case Screen::SgfList: updateSgfList(in); break;
    case Screen::Settings: updateSettings(in); break;
    case Screen::Ogs: updateOgs(in); break;
    }

    if (!m_toast.empty() && nowMs() > m_toastUntil) {
        m_toast.clear();
        m_dirty = true;
    }

    if (m_dirty) {
        gfx_begin();
        gfx_clear(BG);
        switch (m_screen) {
        case Screen::Menu: drawMenu(); break;
        case Screen::Setup: drawSetup(); break;
        case Screen::Game: drawGame(); break;
        case Screen::SgfList: drawSgfList(); break;
        case Screen::Settings: drawSettings(); break;
        case Screen::Ogs: drawOgs(); break;
        }
        if (!m_toast.empty()) {
            int w = gfx_text_width(24, m_toast.c_str()) + 48;
            int x = (SCREEN_W - w) / 2;
            gfx_round_rect(x, 600, w, 56, 28, RGBA(10, 12, 16, 235));
            gfx_text(x + 24, 614, 24, TEXT, m_toast.c_str());
        }
        gfx_end();
        m_dirty = false;
    } else {
        svcSleepThread(16000000ULL);
    }
    return !m_quit;
}

// ------------------------------------------------------------------ menu principal

enum MenuItem { MI_VS_AI, MI_TWO, MI_SANDBOX, MI_SGF, MI_OGS, MI_SETTINGS, MI_QUIT, MI_COUNT };

static Rect menuRect(int i)
{
    return {64, 168 + i * 70, 500, 58};
}

static const char* menuLabel(int i)
{
    switch (i) {
    case MI_VS_AI: return L("Play against KataGo", "Jouer contre KataGo");
    case MI_TWO: return L("Two players (same console)", "Partie à deux (même console)");
    case MI_SANDBOX: return L("Free analysis", "Analyse libre");
    case MI_SGF: return L("Open a game (SGF)", "Ouvrir une partie (SGF)");
    case MI_OGS: return L("Play online (OGS)", "Jouer en ligne (OGS)");
    case MI_SETTINGS: return L("Settings", "Paramètres");
    default: return L("Quit", "Quitter");
    }
}

void App::updateMenu(const Input& in)
{
    if (in.repeated(HidNpadButton_AnyUp))
        m_menuFocus = (m_menuFocus + MI_COUNT - 1) % MI_COUNT;
    if (in.repeated(HidNpadButton_AnyDown))
        m_menuFocus = (m_menuFocus + 1) % MI_COUNT;
    int chosen = -1;
    if (in.pressed(HidNpadButton_A))
        chosen = m_menuFocus;
    for (int i = 0; i < MI_COUNT; i++)
        if (menuRect(i).tapped(in))
            chosen = m_menuFocus = i;
    if (in.pressed(HidNpadButton_Plus))
        m_quit = true;

    switch (chosen) {
    case MI_VS_AI:
        m_setupMode = Mode::VsAI;
        m_setupFocus = 0;
        m_screen = Screen::Setup;
        break;
    case MI_TWO:
        m_setupMode = Mode::TwoPlayers;
        m_setupFocus = 0;
        m_screen = Screen::Setup;
        break;
    case MI_SANDBOX: openSandbox(); break;
    case MI_SGF: {
        m_files.clear();
        const char* dirs[] = {SGF_DIR, "sdmc:/sgf"};
        for (const char* d : dirs)
            if (DIR* dir = opendir(d)) {
                while (dirent* e = readdir(dir)) {
                    std::string name = e->d_name;
                    if (name.size() > 4 && (name.substr(name.size() - 4) == ".sgf" || name.substr(name.size() - 4) == ".SGF"))
                        m_files.push_back(std::string(d) + "/" + name);
                }
                closedir(dir);
            }
        // Les plus récentes d'abord (noms datés)
        std::sort(m_files.begin(), m_files.end(), std::greater<std::string>());
        m_listFocus = 0;
        m_listScroll = 0;
        m_screen = Screen::SgfList;
        break;
    }
    case MI_OGS: m_screen = Screen::Ogs; break;
    case MI_SETTINGS:
        m_settingsFocus = 0;
        m_screen = Screen::Settings;
        break;
    case MI_QUIT: m_quit = true; break;
    default: break;
    }
}

void App::drawMenu()
{
    gfx_text(64, 44, 72, TEXT, "NXGo");
    gfx_text(68, 124, 24, DIM, L("Go with KataGo, on Switch", "Le go avec KataGo, sur Switch"));
    for (int i = 0; i < MI_COUNT; i++) {
        drawButton(menuRect(i), menuLabel(i), nullptr, i == m_menuFocus);
        if (i == MI_OGS) {
            const char* soon = L("Soon", "Bientôt");
            Rect r = menuRect(i);
            int w = gfx_text_width(18, soon) + 24;
            gfx_round_rect(r.x + r.w - w - 16, r.y + 15, w, 28, 14, RGB(90, 70, 30));
            gfx_text(r.x + r.w - w - 4, r.y + 19, 18, WOOD, soon);
        }
    }

    m_demoView.layout(640, 70, 560, 9, false);
    m_demoView.draw(m_demo, BoardDrawOptions());

    std::string status;
    GfxColor sc = DIM;
    if (m_engine.loading())
        status = L("Loading KataGo…", "Chargement de KataGo…");
    else if (!m_engine.ready()) {
        status = strf("%s (%s)", L("KataGo unavailable", "KataGo indisponible"), m_engine.error().c_str());
        sc = DANGER;
    } else {
        std::string nets;
        for (int i = 0; i < (int)NetId::Count; i++)
            if (m_engine.hasNet((NetId)i))
                nets += std::string(nets.empty() ? "" : ", ") + Engine::netName((NetId)i);
        status = strf("%s  ·  %s %s", L("KataGo ready", "KataGo prêt"), L("networks", "réseaux"), nets.c_str());
    }
    gfx_text(66, 690, 20, sc, status.c_str());
    drawHints({{"A", L("Select", "Choisir")}, {"+", L("Quit", "Quitter")}});
}

// ------------------------------------------------------------------ nouvelle partie

enum SetupRow { SR_SIZE, SR_COLOR, SR_LEVEL, SR_HANDICAP, SR_RULES, SR_COUNT };

static const int SIZES[] = {9, 13, 19};

static float autoKomi()
{
    if (g_settings.handicap >= 2)
        return 0.5f;
    return g_settings.rules == 1 ? 7.5f : 6.5f;
}

static bool setupRowVisible(int row, bool vsAI)
{
    return vsAI || (row != SR_COLOR && row != SR_LEVEL);
}

static Rect setupStartRect()
{
    return {120, 560, 500, 62};
}

static Rect setupBackRect()
{
    return {660, 560, 500, 62};
}

void App::updateSetup(const Input& in)
{
    bool vsAI = m_setupMode == Mode::VsAI;
    std::vector<int> rows;
    for (int r = 0; r < SR_COUNT; r++)
        if (setupRowVisible(r, vsAI))
            rows.push_back(r);
    int count = (int)rows.size() + 1;  // + bouton Commencer
    if (in.repeated(HidNpadButton_AnyUp))
        m_setupFocus = (m_setupFocus + count - 1) % count;
    if (in.repeated(HidNpadButton_AnyDown))
        m_setupFocus = (m_setupFocus + 1) % count;

    for (int i = 0; i < (int)rows.size(); i++) {
        ValueRow vr = valueRowRect(i);
        if (vr.row.tapped(in))
            m_setupFocus = i;
        int d = valueRowDelta(in, vr, m_setupFocus == i);
        if (!d)
            continue;
        m_setupFocus = i;
        Settings& s = g_settings;
        switch (rows[i]) {
        case SR_SIZE: {
            int idx = 0;
            for (int k = 0; k < 3; k++)
                if (SIZES[k] == s.boardSize)
                    idx = k;
            s.boardSize = SIZES[(idx + d + 3) % 3];
            break;
        }
        case SR_COLOR: s.color = (s.color + d + 3) % 3; break;
        case SR_LEVEL: s.aiLevel = std::clamp(s.aiLevel + d, AI_LEVEL_MIN, AI_LEVEL_MAX); break;
        case SR_HANDICAP: {
            int h = s.handicap + d;
            if (h == 1)
                h = d > 0 ? 2 : 0;
            s.handicap = std::clamp(h, 0, 9);
            break;
        }
        case SR_RULES: s.rules = 1 - s.rules; break;
        }
    }

    if (setupStartRect().tapped(in) || (in.pressed(HidNpadButton_A) && m_setupFocus == (int)rows.size()) ||
        in.pressed(HidNpadButton_Plus)) {
        settings_save();
        startGame();
        return;
    }
    if (in.pressed(HidNpadButton_A) && m_setupFocus < (int)rows.size())
        m_setupFocus = (int)rows.size();  // A sur une ligne : aller à "Commencer"
    if (setupBackRect().tapped(in) || in.pressed(HidNpadButton_B)) {
        settings_save();
        m_screen = Screen::Menu;
    }
}

void App::drawSetup()
{
    bool vsAI = m_setupMode == Mode::VsAI;
    drawTitle(vsAI ? L("Game against KataGo", "Partie contre KataGo") : L("Two-player game", "Partie à deux"),
              strf("%s %g", L("Komi:", "Komi :"), autoKomi()).c_str());
    const Settings& s = g_settings;
    int i = 0;
    for (int r = 0; r < SR_COUNT; r++) {
        if (!setupRowVisible(r, vsAI))
            continue;
        ValueRow vr = valueRowRect(i);
        bool f = m_setupFocus == i;
        switch (r) {
        case SR_SIZE: drawValueRow(vr, L("Board size", "Taille du goban"), strf("%dx%d", s.boardSize, s.boardSize), f); break;
        case SR_COLOR: {
            const char* colors[] = {L("Black (first)", "Noir (commence)"), L("White", "Blanc"), L("Random", "Au hasard")};
            drawValueRow(vr, L("Your color", "Ta couleur"), colors[s.color], f);
            break;
        }
        case SR_LEVEL:
            drawValueRow(vr, L("KataGo level", "Niveau de KataGo"), strf("%d · %s", s.aiLevel, levelName(s.aiLevel)), f);
            break;
        case SR_HANDICAP:
            drawValueRow(vr, L("Handicap", "Handicap"),
                         s.handicap ? strf("%d %s", s.handicap, L("stones", "pierres")) : std::string(L("None", "Aucun")), f);
            break;
        case SR_RULES:
            drawValueRow(vr, L("Rules", "Règles"), s.rules ? L("Chinese (area)", "Chinoises (surface)")
                                                            : L("Japanese (territory)", "Japonaises (territoire)"), f);
            break;
        }
        i++;
    }
    drawButton(setupStartRect(), L("Start", "Commencer"), "A", m_setupFocus == i);
    drawButton(setupBackRect(), L("Back", "Retour"), "B", false);
    drawHints({{"A", L("Start", "Commencer")}, {"B", L("Back", "Retour")}});
}

// ------------------------------------------------------------------ partie : logique

void App::startGame()
{
    const Settings& s = g_settings;
    GameSetup setup;
    setup.size = s.boardSize;
    setup.rules = s.rules ? RulesKind::Chinese : RulesKind::Japanese;
    setup.handicap = s.handicap;
    setup.komi = autoKomi();
    setup.aiLevel = s.aiLevel;
    if (m_setupMode == Mode::VsAI) {
        bool humanBlack = s.color == 0 || (s.color == 2 && (armGetSystemTick() & 1));
        setup.black = humanBlack ? PlayerKind::Human : PlayerKind::Engine;
        setup.white = humanBlack ? PlayerKind::Engine : PlayerKind::Human;
        std::string ai = strf("KataGo %d", s.aiLevel);
        setup.blackName = humanBlack ? L("Player", "Joueur") : ai;
        setup.whiteName = humanBlack ? ai : L("Player", "Joueur");
    } else {
        setup.black = setup.white = PlayerKind::Human;
    }
    m_mode = m_setupMode;
    m_game.start(setup);
    m_analysisOn = false;
    m_cursor = Location::getLoc(setup.size / 2, setup.size / 2, setup.size);
    m_cursorVisible = false;
    m_aiRequested = m_analysisPos = 0;
    m_snapshot = AnalysisSnapshot();
    m_lowWinrateCount = 0;
    m_paused = false;
    m_engine.stopAll();
    m_screen = Screen::Game;
}

void App::openSandbox()
{
    GameSetup setup;
    setup.size = g_settings.boardSize;
    setup.rules = g_settings.rules ? RulesKind::Chinese : RulesKind::Japanese;
    setup.komi = g_settings.rules ? 7.5f : 6.5f;
    setup.black = setup.white = PlayerKind::Human;
    m_mode = Mode::Sandbox;
    m_game.start(setup);
    m_analysisOn = true;
    m_cursor = Location::getLoc(setup.size / 2, setup.size / 2, setup.size);
    m_aiRequested = m_analysisPos = 0;
    m_snapshot = AnalysisSnapshot();
    m_paused = false;
    m_engine.stopAll();
    m_screen = Screen::Game;
}

bool App::openSgf(const std::string& path)
{
    std::string err;
    m_engine.stopAll();
    if (!m_game.loadSgf(path, err)) {
        toast(strf("%s : %s", L("Cannot read this file", "Lecture impossible"), err.c_str()));
        return false;
    }
    m_mode = Mode::Sandbox;
    m_analysisOn = true;
    m_cursor = Location::getLoc(m_game.size() / 2, m_game.size() / 2, m_game.size());
    m_aiRequested = m_analysisPos = 0;
    m_snapshot = AnalysisSnapshot();
    m_paused = false;
    m_screen = Screen::Game;
    return true;
}

bool App::aiAssistAllowed() const
{
    return m_game.setup().aiAssistAllowed || m_game.finished();
}

bool App::humanCanPlay() const
{
    if (m_paused)
        return false;
    if (m_mode == Mode::Sandbox)
        return !(m_game.finished() && m_game.atEnd());  // en revue : jouer une variante remplace la suite
    if (m_game.finished())
        return false;
    return m_game.atEnd() && !m_game.bothPassed() && m_game.kindToPlay() == PlayerKind::Human;
}

void App::tryPlay(Loc loc)
{
    if (loc == Board::NULL_LOC || !humanCanPlay())
        return;
    if (m_game.board().colors[loc] != C_EMPTY)
        return;
    if (!m_game.play(loc)) {
        toast(L("Illegal move (ko or suicide)", "Coup illégal (ko ou suicide)"));
        return;
    }
    afterMove();
}

void App::doPass()
{
    if (!humanCanPlay())
        return;
    Player p = m_game.toPlay();
    m_game.pass();
    toast(p == P_BLACK ? L("Black passes", "Noir passe") : L("White passes", "Blanc passe"));
    afterMove();
}

void App::afterMove()
{
    m_snapshot.valid = false;
    m_dirty = true;
}

void App::undo()
{
    if (m_mode == Mode::Sandbox) {
        m_game.back();
        m_dirty = true;
        return;
    }
    m_engine.cancelMove();
    m_aiRequested = 0;
    if (m_game.cursor() == 0)
        return;
    if (m_game.finished())
        m_game.resumePlay();
    m_game.back();
    if (m_mode == Mode::VsAI)
        while (m_game.cursor() > 0 && m_game.kindToPlay() != PlayerKind::Human)
            m_game.back();
    m_game.truncate();
    m_lowWinrateCount = 0;
    m_dirty = true;
}

void App::saveSgf()
{
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    mkdir(SGF_DIR, 0777);
    char name[32];
    time_t now = time(NULL);
    strftime(name, sizeof(name), "%Y-%m-%d_%H%M%S.sgf", localtime(&now));
    std::string path = std::string(SGF_DIR) + "/" + name;
    if (m_game.saveSgf(path))
        toast(strf("%s %s", L("Saved:", "Sauvegardé :"), name));
    else
        toast(L("Could not save the game", "Impossible de sauvegarder la partie"));
}

// Fait avancer l'IA et l'analyse ; appelé à chaque image sur l'écran de partie.
void App::stepEngine()
{
    if (!m_engine.ready())
        return;

    // Coup de l'IA reçu ?
    Loc loc;
    uint64_t pid;
    double wr;
    if (m_engine.pollMove(loc, pid, wr)) {
        m_dirty = true;
        if (pid == m_game.positionId() && m_game.kindToPlay() == PlayerKind::Engine && !m_game.finished()) {
            // Abandon si la partie est clairement perdue depuis deux coups
            m_lowWinrateCount = (wr < 0.03 && m_game.moveCount() > 40) ? m_lowWinrateCount + 1 : 0;
            if (m_lowWinrateCount >= 2) {
                m_game.resign(m_game.toPlay());
                toast(L("KataGo resigns", "KataGo abandonne"));
            } else if (loc == Board::PASS_LOC || !m_game.play(loc)) {
                m_game.pass();
                toast(L("KataGo passes", "KataGo passe"));
            }
            afterMove();
        }
    }

    // Au tour de l'IA ?
    bool aiTurn = m_mode == Mode::VsAI && !m_game.finished() && m_game.atEnd() && !m_game.bothPassed() &&
                  m_game.kindToPlay() == PlayerKind::Engine && !m_paused;
    if (aiTurn && !m_engine.thinking() && m_aiRequested != m_game.positionId()) {
        m_engine.requestMove(m_game.board(), m_game.history(), m_game.toPlay(), m_game.setup().aiLevel,
                             m_game.positionId());
        m_aiRequested = m_game.positionId();
        m_analysisPos = 0;
    }

    // Décompte après deux passes : on a besoin du territoire estimé par KataGo
    bool scoring = m_game.bothPassed() && !m_game.finished();

    // Pas d'analyse de la position finale (deux passes) : la partie y est terminée pour KataGo
    bool wantAnalysis = scoring || (m_analysisOn && aiAssistAllowed() && !aiTurn && !m_game.bothPassed());
    if (wantAnalysis && !m_engine.thinking()) {
        if (m_analysisPos != m_game.positionId()) {
            if (scoring) {
                // Analyse de la position avant les deux passes (même plateau, partie pas finie)
                Game before = m_game;
                before.goTo(m_game.cursor() - 2);
                m_engine.startAnalysis(before.board(), before.history(), before.toPlay(), m_game.positionId());
            } else {
                m_engine.startAnalysis(m_game.board(), m_game.history(), m_game.toPlay(), m_game.positionId());
            }
            m_analysisPos = m_game.positionId();
        }
    } else if (!wantAnalysis && m_engine.analyzing()) {
        m_engine.stopAnalysis();
        m_analysisPos = 0;
    }

    if (m_engine.getAnalysis(m_snapshot, m_snapVersion))
        m_dirty = true;

    if (scoring && m_snapshot.valid && m_snapshot.positionId == m_game.positionId() &&
        m_snapshot.visits >= std::min<int64_t>(60, g_settings.analysisVisits)) {
        m_game.scoreWithOwnership(m_snapshot.ownership);
        m_endFocus = 0;
        if (!m_analysisOn)
            m_engine.stopAnalysis();
        m_dirty = true;
    }

    // Animation "réfléchit…"
    if (m_engine.thinking() || m_wasThinking)
        m_dirty = true;
    m_wasThinking = m_engine.thinking();
}

// ------------------------------------------------------------------ partie : entrées

enum GameAction { GA_PASS, GA_UNDO, GA_ANALYSIS, GA_MENU, GA_PREV, GA_NEXT, GA_COUNT };

static std::vector<std::pair<Rect, int>> gameButtons(bool sandbox)
{
    int x = PANEL_X, w = (PANEL_W - 16) / 2, h = 60;
    int y = 470;
    if (sandbox)
        return {{{x, y, w, h}, GA_PREV},
                {{x + w + 16, y, w, h}, GA_NEXT},
                {{x, y + h + 14, w, h}, GA_PASS},
                {{x + w + 16, y + h + 14, w, h}, GA_ANALYSIS},
                {{x, y + 2 * (h + 14), w, h}, GA_MENU}};
    return {{{x, y, w, h}, GA_PASS},
            {{x + w + 16, y, w, h}, GA_UNDO},
            {{x, y + h + 14, w, h}, GA_ANALYSIS},
            {{x + w + 16, y + h + 14, w, h}, GA_MENU}};
}

enum EndAction { EA_REVIEW, EA_SAVE, EA_RESUME, EA_MENU, EA_COUNT };

static std::vector<std::pair<Rect, int>> endButtons(bool canResume)
{
    int x = PANEL_X, w = PANEL_W, h = 56, y = 420;
    std::vector<std::pair<Rect, int>> b = {{{x, y, w, h}, EA_REVIEW}, {{x, y + 66, w, h}, EA_SAVE}};
    if (canResume)
        b.push_back({{x, y + 132, w, h}, EA_RESUME});
    b.push_back({{x, y + (canResume ? 198 : 132), w, h}, EA_MENU});
    return b;
}

void App::updateGame(const Input& in)
{
    stepEngine();

    if (m_paused) {
        updatePause(in);
        return;
    }

    int n = m_game.size();
    m_view.layout(BOARD_X, BOARD_Y, BOARD_SIDE, n, g_settings.showCoords);

    // Fin de partie (après décompte ou abandon) : panneau de choix
    bool showEnd = m_game.finished() && m_mode != Mode::Sandbox;
    if (showEnd) {
        bool canResume = m_game.endReason() == EndReason::Score;
        auto buttons = endButtons(canResume);
        int count = (int)buttons.size();
        if (in.repeated(HidNpadButton_AnyUp))
            m_endFocus = (m_endFocus + count - 1) % count;
        if (in.repeated(HidNpadButton_AnyDown))
            m_endFocus = (m_endFocus + 1) % count;
        int action = -1;
        if (in.pressed(HidNpadButton_A))
            action = buttons[m_endFocus].second;
        for (auto& b : buttons)
            if (b.first.tapped(in))
                action = b.second;
        switch (action) {
        case EA_REVIEW:
            m_mode = Mode::Sandbox;
            m_analysisOn = true;
            m_analysisPos = 0;
            break;
        case EA_SAVE: saveSgf(); break;
        case EA_RESUME:
            // Décompte contesté : on retire les deux passes et on continue
            m_game.resumePlay();
            m_game.back();
            m_game.back();
            m_game.truncate();
            m_aiRequested = 0;
            break;
        case EA_MENU:
            m_engine.stopAll();
            m_screen = Screen::Menu;
            break;
        default: break;
        }
        if (in.pressed(HidNpadButton_B)) {
            m_engine.stopAll();
            m_screen = Screen::Menu;
        }
        return;
    }

    // Plateau : tactile
    if (in.touchStarted)
        m_touchOnBoard = m_view.contains(in.x, in.y);
    if (in.touching && m_touchOnBoard) {
        m_cursorVisible = false;
        m_touchLoc = m_view.hitTest(in.x, in.y);
    }
    if (in.released && m_touchOnBoard) {
        Loc loc = m_view.hitTest(in.x, in.y);
        m_touchLoc = Board::NULL_LOC;
        m_touchOnBoard = false;
        if (loc != Board::NULL_LOC) {
            m_cursor = loc;
            tryPlay(loc);
        }
    }

    // Plateau : manette
    int cx = Location::getX(m_cursor, n), cy = Location::getY(m_cursor, n);
    int dx = 0, dy = 0;
    if (in.repeated(HidNpadButton_AnyLeft))
        dx = -1;
    if (in.repeated(HidNpadButton_AnyRight))
        dx = 1;
    if (in.repeated(HidNpadButton_AnyUp))
        dy = -1;
    if (in.repeated(HidNpadButton_AnyDown))
        dy = 1;
    if (dx || dy) {
        if (m_cursorVisible) {
            cx = std::clamp(cx + dx, 0, n - 1);
            cy = std::clamp(cy + dy, 0, n - 1);
        }
        m_cursor = Location::getLoc(cx, cy, n);
        m_cursorVisible = true;
    }
    if (in.pressed(HidNpadButton_A)) {
        if (m_cursorVisible)
            tryPlay(m_cursor);
        m_cursorVisible = true;
    }

    // Boutons (manette + tactile)
    int action = -1;
    if (in.pressed(HidNpadButton_Y))
        action = GA_PASS;
    if (in.pressed(HidNpadButton_X))
        action = GA_ANALYSIS;
    if (in.pressed(HidNpadButton_Plus))
        action = GA_MENU;
    if (m_mode == Mode::Sandbox) {
        if (in.repeated(HidNpadButton_L))
            action = GA_PREV;
        if (in.repeated(HidNpadButton_R))
            action = GA_NEXT;
        if (in.pressed(HidNpadButton_ZL)) {
            m_game.goTo(m_game.cursor() - 10);
        }
        if (in.pressed(HidNpadButton_ZR)) {
            m_game.goTo(m_game.cursor() + 10);
        }
    } else if (in.pressed(HidNpadButton_L) || in.pressed(HidNpadButton_B)) {
        action = GA_UNDO;
    }
    for (auto& b : gameButtons(m_mode == Mode::Sandbox))
        if (b.first.tapped(in))
            action = b.second;

    switch (action) {
    case GA_PASS: doPass(); break;
    case GA_UNDO: undo(); break;
    case GA_PREV: m_game.back(); break;
    case GA_NEXT: m_game.forward(); break;
    case GA_ANALYSIS:
        if (!aiAssistAllowed()) {
            toast(L("Analysis is not allowed during an online game", "Analyse interdite pendant une partie en ligne"));
        } else {
            m_analysisOn = !m_analysisOn;
            m_analysisPos = 0;
            if (!m_analysisOn)
                m_engine.stopAnalysis();
        }
        break;
    case GA_MENU:
        m_paused = true;
        m_pauseFocus = 0;
        break;
    default: break;
    }
}

// ------------------------------------------------------------------ partie : dessin

static std::string playerName(const Game& g, Player p, int mode)
{
    const GameSetup& s = g.setup();
    PlayerKind k = g.kindOf(p);
    if (k == PlayerKind::Engine)
        return strf("KataGo · %s %d", L("level", "niveau"), s.aiLevel);
    const std::string& name = p == P_BLACK ? s.blackName : s.whiteName;
    if (mode == 0 && k == PlayerKind::Human)
        return L("You", "Toi");
    if (!name.empty())
        return name;
    return p == P_BLACK ? L("Black", "Noir") : L("White", "Blanc");
}

void App::drawGame()
{
    int n = m_game.size();
    m_view.layout(BOARD_X, BOARD_Y, BOARD_SIDE, n, g_settings.showCoords);

    bool snapshotFresh = m_snapshot.valid && m_snapshot.positionId == m_game.positionId();
    // En revue, la partie peut être finie : on analyse quand même les coups précédents
    bool reviewing = m_mode == Mode::Sandbox;
    bool showAnalysis = m_analysisOn && aiAssistAllowed() && snapshotFresh && (!m_game.finished() || reviewing) &&
                        !(m_game.finished() && m_game.atEnd());

    BoardDrawOptions opt;
    opt.showCoords = g_settings.showCoords;
    if (m_cursorVisible && !m_paused)
        opt.cursor = m_cursor;
    Loc ghost = m_touchLoc != Board::NULL_LOC ? m_touchLoc : (m_cursorVisible ? m_cursor : Board::NULL_LOC);
    if (humanCanPlay() && ghost != Board::NULL_LOC) {
        opt.ghost = ghost;
        opt.ghostLegal = m_game.isLegal(ghost);
    }
    if (showAnalysis) {
        opt.analysis = &m_snapshot;
        opt.showOwnership = g_settings.showOwnership;
    }
    if (m_game.finished() && m_game.score().valid && m_game.atEnd())
        opt.score = &m_game.score();
    m_view.draw(m_game, opt);

    // Cartes des joueurs
    int modeIdx = m_mode == Mode::VsAI ? 0 : (m_mode == Mode::TwoPlayers ? 1 : 2);
    for (int i = 0; i < 2; i++) {
        Player p = i == 0 ? P_BLACK : P_WHITE;
        int y = 16 + i * 100;
        bool toPlay = m_game.toPlay() == p && !m_game.finished();
        gfx_round_rect(PANEL_X, y, PANEL_W, 88, 16, toPlay ? PANEL_HI : PANEL);
        if (toPlay)
            gfx_round_rect(PANEL_X, y, 6, 88, 3, ACCENT);
        BoardView::drawStoneIcon(PANEL_X + 46, y + 44, 24, p);
        gfx_text_fit(PANEL_X + 86, y + 14, 26, TEXT, playerName(m_game, p, modeIdx).c_str(), PANEL_W - 230);
        int prisoners = p == P_BLACK ? m_game.board().numWhiteCaptures : m_game.board().numBlackCaptures;
        std::string sub = strf("%s %d", L("Prisoners:", "Prisonniers :"), prisoners);
        if (p == P_WHITE)
            sub += strf("   ·   komi %g", m_game.setup().komi);
        gfx_text(PANEL_X + 86, y + 50, 20, DIM, sub.c_str());
        if (toPlay) {
            std::string st;
            if (m_game.kindOf(p) == PlayerKind::Engine && m_engine.thinking()) {
                int dots = (int)(nowMs() / 400 % 4);
                st = std::string(L("thinking", "réfléchit")) + std::string(dots, '.');
            } else if (m_game.kindOf(p) == PlayerKind::Engine && m_engine.loading()) {
                st = L("loading…", "chargement…");
            } else {
                st = L("to play", "à jouer");
            }
            gfx_text(PANEL_X + PANEL_W - 24 - gfx_text_width(20, st.c_str()), y + 16, 20, ACCENT, st.c_str());
        }
    }

    // Fin de partie
    if (m_game.finished() && m_mode != Mode::Sandbox) {
        drawEndPanel(PANEL_X, 228, PANEL_W);
        drawHints({{"A", L("Select", "Choisir")}, {"B", L("Menu", "Menu")}});
        return;
    }

    // Analyse : taux de victoire et avance
    int ay = 228;
    gfx_round_rect(PANEL_X, ay, PANEL_W, 132, 16, PANEL);
    if (showAnalysis) {
        double wb = m_snapshot.winrateBlack;
        int bx = PANEL_X + 20, bw = PANEL_W - 40, by = ay + 18;
        int split = (int)(bw * wb);
        gfx_round_rect(bx, by, bw, 34, 10, RGB(240, 240, 236));
        gfx_round_rect(bx, by, std::max(split, 10), 34, 10, RGB(18, 18, 22));
        std::string lb = strf("%.1f%%", wb * 100), lw = strf("%.1f%%", (1 - wb) * 100);
        gfx_text(bx + 12, by + 5, 22, RGB(240, 240, 236), lb.c_str());
        gfx_text(bx + bw - 12 - gfx_text_width(22, lw.c_str()), by + 5, 22, RGB(18, 18, 22), lw.c_str());
        double lead = m_snapshot.leadBlack;
        std::string who = lead >= 0 ? L("Black", "Noir") : L("White", "Blanc");
        std::string l1 = strf("%s %s %.1f %s", who.c_str(), L("leads by", "mène de"), std::fabs(lead), L("pts", "pts"));
        gfx_text(PANEL_X + 20, ay + 64, 24, TEXT, l1.c_str());
        std::string l2 = strf("%lld %s  ·  %s %s", (long long)m_snapshot.visits, L("visits", "visites"), L("network", "réseau"),
                              Engine::netName(m_engine.analysisNet()));
        if (!m_snapshot.moves.empty())
            l2 += strf("  ·  %s %s", L("best", "meilleur"), Game::coordString(m_snapshot.moves[0].loc, n).c_str());
        gfx_text(PANEL_X + 20, ay + 98, 20, DIM, l2.c_str());
    } else {
        const char* msg;
        if (!aiAssistAllowed())
            msg = L("Analysis disabled during online games", "Analyse désactivée en partie en ligne");
        else if (m_game.finished() && m_game.atEnd())
            msg = L("Game over: press L to replay it.", "Partie terminée : L pour la revoir.");
        else if (m_game.bothPassed())
            msg = L("Counting the score…", "Décompte des points…");
        else if (m_engine.loading())
            msg = L("Loading KataGo…", "Chargement de KataGo…");
        else if (m_analysisOn)
            msg = L("Analysing…", "Analyse en cours…");
        else
            msg = L("Press X for KataGo's analysis", "Appuie sur X pour l'analyse de KataGo");
        gfx_text_fit(PANEL_X + 20, ay + 52, 22, DIM, msg, PANEL_W - 40);
    }

    // Coup courant
    std::string info;
    if (m_mode == Mode::Sandbox)
        info = strf("%s %d / %d", L("Move", "Coup"), m_game.cursor(), m_game.moveCount());
    else
        info = strf("%s %d", L("Move", "Coup"), m_game.cursor());
    Loc last = m_game.lastMove();
    if (last != Board::NULL_LOC) {
        const char* who = m_game.lastMovePlayer() == P_BLACK ? L("Black", "Noir") : L("White", "Blanc");
        if (last == Board::PASS_LOC)
            info += strf("   ·   %s %s", who, L("passed", "a passé"));
        else
            info += strf("   ·   %s %s", who, Game::coordString(last, n).c_str());
    }
    gfx_text(PANEL_X + 4, 386, 24, TEXT, info.c_str());
    if (m_game.finished())
        gfx_text(PANEL_X + 4, 422, 22, WOOD, strf("%s %s", L("Result:", "Résultat :"), m_game.resultString().c_str()).c_str());

    // Boutons
    bool sandbox = m_mode == Mode::Sandbox;
    for (auto& b : gameButtons(sandbox)) {
        switch (b.second) {
        case GA_PASS: drawButton(b.first, L("Pass", "Passer"), "Y", false, humanCanPlay()); break;
        case GA_UNDO: drawButton(b.first, L("Undo", "Annuler"), "L", false, m_game.cursor() > 0); break;
        case GA_ANALYSIS: drawButton(b.first, L("Analysis", "Analyse"), "X", false, aiAssistAllowed(), m_analysisOn); break;
        case GA_MENU: drawButton(b.first, L("Menu", "Menu"), "+", false); break;
        case GA_PREV: drawButton(b.first, L("Previous", "Précédent"), "L", false, m_game.cursor() > 0); break;
        case GA_NEXT: drawButton(b.first, L("Next", "Suivant"), "R", false, !m_game.atEnd()); break;
        }
    }

    if (sandbox)
        drawHints({{"A", L("Play", "Jouer")}, {"L/R", L("Moves", "Coups")}, {"ZL/ZR", "±10"}});
    else
        drawHints({{"A", L("Play", "Jouer")}, {"Y", L("Pass", "Passer")}, {"L", L("Undo", "Annuler")}});

    if (m_paused)
        drawPause();
}

void App::drawEndPanel(int x, int y, int w)
{
    gfx_round_rect(x, y, w, 176, 16, PANEL);
    std::string title;
    Player win = m_game.winner();
    const ScoreResult& s = m_game.score();
    if (m_game.endReason() == EndReason::Resign)
        title = win == P_BLACK ? L("Black wins by resignation", "Noir gagne par abandon")
                               : L("White wins by resignation", "Blanc gagne par abandon");
    else if (win == C_EMPTY)
        title = L("Draw", "Égalité");
    else
        title = strf("%s %g %s", win == P_BLACK ? L("Black wins by", "Noir gagne de") : L("White wins by", "Blanc gagne de"),
                     std::fabs(s.blackScore - s.whiteScore), L("points", "points"));
    gfx_text_fit(x + 20, y + 18, 30, WOOD, title.c_str(), w - 40);
    if (m_game.endReason() == EndReason::Score && s.valid) {
        bool area = m_game.setup().rules == RulesKind::Chinese;
        std::string lb, lw;
        if (area) {
            lb = strf("%s %g", L("Black: area", "Noir : surface"), s.blackScore);
            lw = strf("%s %g (%s %g)", L("White: area", "Blanc : surface"), s.whiteScore, L("incl. komi", "dont komi"),
                      m_game.setup().komi);
        } else {
            lb = strf("%s %d + %d %s = %g", L("Black:", "Noir :"), s.blackTerritory, s.blackPrisoners,
                      L("prisoners", "prisonniers"), s.blackScore);
            lw = strf("%s %d + %d %s + %g = %g", L("White:", "Blanc :"), s.whiteTerritory, s.whitePrisoners,
                      L("prisoners", "prisonniers"), m_game.setup().komi, s.whiteScore);
        }
        gfx_text_fit(x + 20, y + 68, 22, TEXT, lb.c_str(), w - 40);
        gfx_text_fit(x + 20, y + 100, 22, TEXT, lw.c_str(), w - 40);
        gfx_text_fit(x + 20, y + 136, 18, DIM, L("Dead stones estimated by KataGo.", "Pierres mortes estimées par KataGo."),
                     w - 40);
    }
    bool canResume = m_game.endReason() == EndReason::Score;
    for (auto& b : endButtons(canResume)) {
        bool f = false;
        auto all = endButtons(canResume);
        for (int i = 0; i < (int)all.size(); i++)
            if (all[i].second == b.second)
                f = i == m_endFocus;
        switch (b.second) {
        case EA_REVIEW: drawButton(b.first, L("Review with analysis", "Revoir avec l'analyse"), nullptr, f); break;
        case EA_SAVE: drawButton(b.first, L("Save (SGF)", "Sauvegarder (SGF)"), nullptr, f); break;
        case EA_RESUME: drawButton(b.first, L("Resume the game", "Reprendre la partie"), nullptr, f); break;
        case EA_MENU: drawButton(b.first, L("Main menu", "Menu principal"), nullptr, f); break;
        }
    }
}

// ------------------------------------------------------------------ menu pause

enum PauseItem { PI_RESUME, PI_SAVE, PI_RESIGN, PI_MENU, PI_COUNT };

static Rect pauseRect(int i)
{
    return {390, 196 + i * 76, 500, 62};
}

void App::updatePause(const Input& in)
{
    bool canResign = m_mode != Mode::Sandbox && !m_game.finished();
    if (in.repeated(HidNpadButton_AnyUp))
        m_pauseFocus = (m_pauseFocus + PI_COUNT - 1) % PI_COUNT;
    if (in.repeated(HidNpadButton_AnyDown))
        m_pauseFocus = (m_pauseFocus + 1) % PI_COUNT;
    int chosen = -1;
    if (in.pressed(HidNpadButton_A))
        chosen = m_pauseFocus;
    for (int i = 0; i < PI_COUNT; i++)
        if (pauseRect(i).tapped(in))
            chosen = i;
    if (in.pressed(HidNpadButton_B) || in.pressed(HidNpadButton_Plus))
        chosen = PI_RESUME;
    switch (chosen) {
    case PI_RESUME: m_paused = false; break;
    case PI_SAVE:
        saveSgf();
        m_paused = false;
        break;
    case PI_RESIGN:
        if (!canResign)
            break;
        m_engine.cancelMove();
        // Contre l'IA, c'est le joueur humain qui abandonne ; à deux, celui qui a le trait.
        if (m_mode == Mode::VsAI)
            m_game.resign(m_game.setup().black == PlayerKind::Human ? P_BLACK : P_WHITE);
        else
            m_game.resign(m_game.toPlay());
        m_endFocus = 0;
        m_paused = false;
        break;
    case PI_MENU:
        m_engine.stopAll();
        m_paused = false;
        m_screen = Screen::Menu;
        break;
    default: break;
    }
}

void App::drawPause()
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 150));
    gfx_round_rect(360, 110, 560, 500, 24, RGB(30, 34, 43));
    gfx_text(390, 136, 32, TEXT, L("Pause", "Pause"));
    bool canResign = m_mode != Mode::Sandbox && !m_game.finished();
    const char* labels[PI_COUNT] = {L("Resume", "Reprendre"), L("Save the game (SGF)", "Sauvegarder la partie (SGF)"),
                                    L("Resign", "Abandonner"), L("Main menu", "Menu principal")};
    for (int i = 0; i < PI_COUNT; i++)
        drawButton(pauseRect(i), labels[i], nullptr, i == m_pauseFocus, i != PI_RESIGN || canResign);
}

// ------------------------------------------------------------------ liste SGF

static const int LIST_TOP = 140, LIST_ROW = 66, LIST_H = 520;

void App::updateSgfList(const Input& in)
{
    int count = (int)m_files.size();
    if (in.pressed(HidNpadButton_B)) {
        m_screen = Screen::Menu;
        return;
    }
    if (count == 0)
        return;
    if (in.repeated(HidNpadButton_AnyUp))
        m_listFocus = std::max(0, m_listFocus - 1);
    if (in.repeated(HidNpadButton_AnyDown))
        m_listFocus = std::min(count - 1, m_listFocus + 1);
    // Garder l'élément choisi visible
    float maxScroll = std::max(0, count * LIST_ROW - LIST_H);
    if (in.down & (HidNpadButton_AnyUp | HidNpadButton_AnyDown)) {
        float top = m_listFocus * LIST_ROW, bottom = top + LIST_ROW;
        if (top < m_listScroll)
            m_listScroll = top;
        if (bottom > m_listScroll + LIST_H)
            m_listScroll = bottom - LIST_H;
    }
    // Défilement au doigt
    if (in.touching && in.touchIn(0, LIST_TOP, SCREEN_W, LIST_H))
        m_listScroll -= in.dragY;
    m_listScroll = std::clamp(m_listScroll, 0.0f, maxScroll);

    int chosen = -1;
    if (in.pressed(HidNpadButton_A))
        chosen = m_listFocus;
    if (in.tap && in.y >= LIST_TOP && in.y < LIST_TOP + LIST_H) {
        int i = (int)((in.y - LIST_TOP + m_listScroll) / LIST_ROW);
        if (i >= 0 && i < count)
            chosen = m_listFocus = i;
    }
    if (chosen >= 0)
        openSgf(m_files[chosen]);
}

void App::drawSgfList()
{
    drawTitle(L("Open a game", "Ouvrir une partie"),
              L("SGF files from /switch/nxgo/sgf and /sgf on the SD card", "Fichiers SGF de /switch/nxgo/sgf et /sgf sur la carte SD"));
    if (m_files.empty()) {
        gfx_text(120, 220, 26, DIM, L("No SGF file found.", "Aucun fichier SGF trouvé."));
        gfx_text(120, 262, 22, DIM,
                 L("Save a game from the pause menu, or copy .sgf files to the SD card.",
                   "Sauvegarde une partie depuis le menu pause, ou copie des fichiers .sgf sur la carte SD."));
    }
    gfx_set_clip(0, LIST_TOP, SCREEN_W, LIST_H);
    for (int i = 0; i < (int)m_files.size(); i++) {
        int y = LIST_TOP + i * LIST_ROW - (int)m_listScroll;
        if (y + LIST_ROW < LIST_TOP || y > LIST_TOP + LIST_H)
            continue;
        Rect r = {120, y + 4, 1040, LIST_ROW - 8};
        bool f = i == m_listFocus;
        gfx_round_rect(r.x, r.y, r.w, r.h, 14, f ? PANEL_HI : PANEL);
        if (f)
            gfx_round_rect(r.x, r.y, 6, r.h, 3, ACCENT);
        const std::string& p = m_files[i];
        size_t slash = p.find_last_of('/');
        std::string name = p.substr(slash + 1), dir = p.substr(0, slash);
        gfx_text_fit(r.x + 28, r.y + 15, 26, TEXT, name.c_str(), 700);
        gfx_text(r.x + r.w - 24 - gfx_text_width(18, dir.c_str() + 5), r.y + 20, 18, DIM, dir.c_str() + 5);
    }
    gfx_reset_clip();
    drawHints({{"A", L("Open", "Ouvrir")}, {"B", L("Back", "Retour")}});
}

// ------------------------------------------------------------------ réglages

enum SettingRow { ST_LANG, ST_NET, ST_VISITS, ST_COORDS, ST_OWNERSHIP, ST_COUNT };

static const int VISITS[] = {100, 200, 400, 800, 1600, 5000};

void App::updateSettings(const Input& in)
{
    if (in.repeated(HidNpadButton_AnyUp))
        m_settingsFocus = (m_settingsFocus + ST_COUNT - 1) % ST_COUNT;
    if (in.repeated(HidNpadButton_AnyDown))
        m_settingsFocus = (m_settingsFocus + 1) % ST_COUNT;
    Settings& s = g_settings;
    for (int i = 0; i < ST_COUNT; i++) {
        ValueRow vr = valueRowRect(i);
        if (vr.row.tapped(in))
            m_settingsFocus = i;
        int d = valueRowDelta(in, vr, m_settingsFocus == i);
        if (!d && !(in.pressed(HidNpadButton_A) && m_settingsFocus == i && (i == ST_COORDS || i == ST_OWNERSHIP || i == ST_LANG)))
            continue;
        if (!d)
            d = 1;
        switch (i) {
        case ST_LANG: s.lang = s.lang == Lang::FR ? Lang::EN : Lang::FR; break;
        case ST_NET: {
            // Réseaux disponibles seulement
            int net = s.analysisNet;
            for (int k = 0; k < (int)NetId::Count; k++) {
                net = (net + d + (int)NetId::Count) % (int)NetId::Count;
                if (m_engine.hasNet((NetId)net))
                    break;
            }
            s.analysisNet = net;
            m_engine.setAnalysisNet((NetId)net);
            break;
        }
        case ST_VISITS: {
            int idx = 2, cnt = sizeof(VISITS) / sizeof(VISITS[0]);
            for (int k = 0; k < cnt; k++)
                if (VISITS[k] == s.analysisVisits)
                    idx = k;
            s.analysisVisits = VISITS[std::clamp(idx + d, 0, cnt - 1)];
            m_engine.setAnalysisMaxVisits(s.analysisVisits);
            break;
        }
        case ST_COORDS: s.showCoords = !s.showCoords; break;
        case ST_OWNERSHIP: s.showOwnership = !s.showOwnership; break;
        }
    }
    if (in.pressed(HidNpadButton_B) || in.pressed(HidNpadButton_Plus)) {
        settings_save();
        m_screen = Screen::Menu;
    }
}

void App::drawSettings()
{
    drawTitle(L("Settings", "Paramètres"));
    const Settings& s = g_settings;
    const char* yes = L("Shown", "Affichées");
    const char* no = L("Hidden", "Masquées");
    drawValueRow(valueRowRect(ST_LANG), L("Language", "Langue"), s.lang == Lang::FR ? "Français" : "English",
                 m_settingsFocus == ST_LANG);
    std::string net = Engine::netName((NetId)s.analysisNet);
    if (s.analysisNet == 0)
        net += L(" (fast)", " (rapide)");
    else if (s.analysisNet == 2)
        net += L(" (strong, slow)", " (fort, lent)");
    drawValueRow(valueRowRect(ST_NET), L("Analysis network", "Réseau d'analyse"), net, m_settingsFocus == ST_NET,
                 m_engine.ready());
    drawValueRow(valueRowRect(ST_VISITS), L("Analysis depth", "Profondeur d'analyse"),
                 strf("%d %s", s.analysisVisits, L("visits", "visites")), m_settingsFocus == ST_VISITS);
    drawValueRow(valueRowRect(ST_COORDS), L("Coordinates", "Coordonnées"), s.showCoords ? yes : no,
                 m_settingsFocus == ST_COORDS);
    drawValueRow(valueRowRect(ST_OWNERSHIP), L("Territory (analysis)", "Territoire (analyse)"),
                 s.showOwnership ? L("Shown", "Affiché") : L("Hidden", "Masqué"), m_settingsFocus == ST_OWNERSHIP);
    gfx_text(124, 540, 20, DIM,
             L("Tip: extra networks (b6, b10, b15 .bin.gz or .txt.gz) can be put in /switch/nxgo/models.",
               "Astuce : d'autres réseaux (b6, b10, b15 en .bin.gz ou .txt.gz) se mettent dans /switch/nxgo/models."));
    drawHints({{"←→", L("Change", "Modifier")}, {"B", L("Back", "Retour")}});
}

// ------------------------------------------------------------------ OGS

void App::updateOgs(const Input& in)
{
    if (in.pressed(HidNpadButton_B) || in.pressed(HidNpadButton_A) || Rect{120, 560, 500, 62}.tapped(in))
        m_screen = Screen::Menu;
}

void App::drawOgs()
{
    drawTitle(L("Play online on OGS", "Jouer en ligne sur OGS"), "online-go.com");
    const char* lines[] = {
        L("Coming soon:", "Bientôt :"),
        L("·  sign in with your online-go.com account by scanning a QR code,", "·  connexion avec ton compte online-go.com en scannant un QR code,"),
        L("·  ranked and casual games against players worldwide,", "·  parties classées et amicales contre des joueurs du monde entier,"),
        L("·  your finished games reviewed with KataGo.", "·  tes parties terminées revues avec KataGo."),
        "",
        L("During an online game, KataGo's analysis is disabled: OGS forbids", "Pendant une partie en ligne, l'analyse de KataGo est désactivée : OGS interdit"),
        L("any AI help while a game is in progress.", "toute aide d'une IA tant que la partie n'est pas finie."),
    };
    int y = 160;
    for (const char* l : lines) {
        gfx_text(124, y, 24, l == lines[0] ? WOOD : TEXT, l);
        y += 42;
    }
    drawButton({120, 560, 500, 62}, L("Back", "Retour"), "B", true);
    drawHints({{"B", L("Back", "Retour")}});
}
