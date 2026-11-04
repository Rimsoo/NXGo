// Écrans en ligne (online-go.com) : connexion, parties en cours, recherche d'adversaire,
// et la partie elle-même. Pendant une partie en ligne, KataGo ne sert jamais : ni analyse,
// ni indice, ni estimation des pierres mortes (règle d'OGS). La revue avec analyse n'est
// proposée qu'une fois la partie terminée.

#include <algorithm>
#include <cmath>

#include "gfx.h"
#include "log.h"
#include "settings.h"
#include "ui.h"
#include "ui_widgets.h"

using json = nlohmann::json;

static const char* SIZE_KEYS[] = {"9x9", "13x13", "19x19"};
static const char* SPEED_KEYS[] = {"blitz", "rapid", "live", "correspondence"};

static const char* speedName(const std::string& key)
{
    if (key == "blitz")
        return L("Blitz", "Blitz");
    if (key == "rapid")
        return L("Rapid", "Rapide");
    if (key == "live")
        return L("Live", "Normale");
    if (key == "correspondence")
        return L("Correspondence", "Par correspondance");
    return key.c_str();
}

// Rang OGS : 0 = 30k … 29 = 1k, 30 = 1d.
static std::string rankString(double rank)
{
    int r = (int)std::floor(rank);
    if (r < 0)
        return "?";
    return r >= 30 ? strf("%dd", r - 29) : strf("%dk", 30 - r);
}

static std::string fmtDuration(double sec)
{
    if (sec < 0)
        sec = 0;
    long s = (long)std::ceil(sec);
    if (s >= 86400)
        return strf("%ld%s %ldh", s / 86400, L("d", "j"), s % 86400 / 3600);
    if (s >= 3600)
        return strf("%ldh%02ld", s / 3600, s % 3600 / 60);
    return strf("%ld:%02ld", s / 60, s % 60);
}

static void parseCoords(const std::string& s, int n, std::vector<bool>& out)
{
    out.assign(n * n, false);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int x = s[i] - 'a', y = s[i + 1] - 'a';
        if (x >= 0 && x < n && y >= 0 && y < n)
            out[y * n + x] = true;
    }
}

static bool askText(const char* header, bool password, std::string& inout)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    if (password)
        swkbdConfigMakePresetPassword(&kbd);
    else
        swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, header);
    swkbdConfigSetStringLenMax(&kbd, 128);
    if (!password && !inout.empty())
        swkbdConfigSetInitialText(&kbd, inout.c_str());
    char buf[256] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_FAILED(rc) || !buf[0])
        return false;
    inout = buf;
    return true;
}

// ------------------------------------------------------------------ événements

void App::ogsStep()
{
    OgsEvent ev;
    while (m_ogs.pollEvent(ev)) {
        m_dirty = true;
        if (ev.name == "automatch/start") {
            int64_t id = ev.data.value("game_id", (int64_t)0);
            if (id)
                openOnlineGame(id);
            continue;
        }
        if (ev.name == "sgf/ready") {
            // Partie terminée téléchargée : revue avec analyse et courbe de score
            if (m_screen == Screen::Ogs && openSgf(ev.data.value("path", ""))) {
                m_analysisOn = true;
                m_backToOgs = true;
            }
            continue;
        }
        if (ev.name == "sgf/error") {
            toast(strf("%s (%s)", L("Download failed", "Échec du téléchargement"), ev.data.value("message", "").c_str()));
            continue;
        }
        if (ev.name == "automatch/cancel") {
            toast(L("Opponent search stopped", "Recherche d'adversaire arrêtée"));
            continue;
        }
        // game/<id>/<type>
        size_t slash = ev.name.find('/', 5);
        if (slash == std::string::npos)
            continue;
        int64_t id = atoll(ev.name.substr(5, slash - 5).c_str());
        if (m_mode != Mode::Online || id != m_online.gameId)
            continue;
        try {
            ogsGameEvent(ev.name.substr(slash + 1), ev.data);
        } catch (const std::exception& e) {
            log_printf("OGS : événement %s illisible (%s)\n", ev.name.c_str(), e.what());
        }
    }
}

void App::openOnlineGame(int64_t id)
{
    m_engine.stopAll();
    if (m_mode == Mode::Online && m_online.gameId && m_online.gameId != id)
        m_ogs.gameDisconnect(m_online.gameId);
    m_online = Online();
    m_online.gameId = id;
    GameSetup blank;
    blank.black = blank.white = PlayerKind::Remote;
    blank.aiAssistAllowed = false;
    m_game.start(blank);
    m_mode = Mode::Online;
    m_analysisOn = false;
    m_paused = false;
    m_cursorVisible = false;
    m_snapshot = AnalysisSnapshot();
    m_screen = Screen::Game;
    m_ogs.gameConnect(id);
}

void App::ogsLeaveGame()
{
    if (m_online.gameId)
        m_ogs.gameDisconnect(m_online.gameId);
    m_online = Online();
    m_mode = Mode::Sandbox;
    m_paused = false;
    m_screen = Screen::Ogs;
    m_ogs.refreshGames();
}

void App::ogsGameEvent(const std::string& kind, const json& d)
{
    Online& o = m_online;
    if (kind == "gamedata") {
        ogsLoadGamedata(d);
    } else if (!o.loaded) {
        return;  // le reste n'a de sens qu'après gamedata
    } else if (kind == "move") {
        ogsApplyMove(d);
    } else if (kind == "clock") {
        ogsSetClock(d);
    } else if (kind == "phase") {
        std::string phase = d.is_string() ? d.get<std::string>() : "";
        o.phase = phase;
        o.movePending = false;
        o.oppAccepted = o.iAccepted = false;
        if (phase == "stone removal") {
            ogsSetRemoved(o.removed);
        } else if (phase == "play") {
            o.removed.clear();
            toast(L("Back to the game", "Retour à la partie"));
        } else if (phase == "finished") {
            m_ogs.gameConnect(o.gameId);  // le serveur renvoie gamedata avec le résultat
        }
    } else if (kind == "removed_stones") {
        if (d.is_object() && d.contains("all_removed") && d["all_removed"].is_string()) {
            ogsSetRemoved(d["all_removed"].get<std::string>());
            o.oppAccepted = o.iAccepted = false;
        }
    } else if (kind == "removed_stones_accepted") {
        if (d.value("player_id", (int64_t)0) == m_ogs.userId())
            o.iAccepted = true;
        else
            o.oppAccepted = true;
        if (d.value("phase", "") == "finished")
            m_ogs.gameConnect(o.gameId);
    } else if (kind == "undo_requested") {
        toast(L("Your opponent asked for an undo (not supported here)",
                "Ton adversaire demande d'annuler un coup (non géré ici)"));
    } else if (kind == "error") {
        toast(d.is_string() ? d.get<std::string>() : d.dump());
        o.movePending = false;
    } else if (kind == "auto_resign") {
        toast(L("Your opponent left: they will lose if they don't come back",
                "Ton adversaire est parti : il perdra s'il ne revient pas"));
    }
}

void App::ogsLoadGamedata(const json& d)
{
    Online& o = m_online;
    int w = d.value("width", 19), h = d.value("height", 19);
    if (w != h || w < 2 || w > 19) {
        toast(L("Board size not supported", "Taille de goban non gérée"));
        ogsLeaveGame();
        return;
    }
    o.blackId = d.value("black_player_id", (int64_t)0);
    o.whiteId = d.value("white_player_id", (int64_t)0);
    int64_t me = m_ogs.userId();
    o.me = me == o.blackId ? P_BLACK : (me == o.whiteId ? P_WHITE : C_EMPTY);
    o.blackName = o.whiteName = "";
    if (d.contains("players") && d["players"].is_object()) {
        for (int i = 0; i < 2; i++) {
            const char* key = i == 0 ? "black" : "white";
            if (!d["players"].contains(key) || !d["players"][key].is_object())
                continue;
            const json& p = d["players"][key];
            std::string name = p.value("username", "");
            const char* rk = p.contains("rank") ? "rank" : "ranking";
            if (p.contains(rk) && p[rk].is_number() && p[rk].get<double>() > 0)
                name += " [" + rankString(p[rk].get<double>()) + "]";
            (i == 0 ? o.blackName : o.whiteName) = name;
        }
    }
    std::string rules = d.value("rules", "japanese");
    GameSetup s;
    s.size = w;
    s.rules = (rules == "japanese" || rules == "korean") ? RulesKind::Japanese : RulesKind::Chinese;
    s.komi = d.value("komi", 6.5f);
    s.handicap = d.value("handicap", 0);
    s.black = o.me == P_BLACK ? PlayerKind::Human : PlayerKind::Remote;
    s.white = o.me == P_WHITE ? PlayerKind::Human : PlayerKind::Remote;
    s.blackName = o.blackName;
    s.whiteName = o.whiteName;
    s.aiAssistAllowed = false;
    o.handicap = s.handicap;
    o.freeHandicap = d.value("free_handicap_placement", false);
    o.ranked = d.value("ranked", false);
    if (d.contains("time_control") && d["time_control"].is_object()) {
        const json& tc = d["time_control"];
        o.system = tc.value("system", tc.value("time_control", ""));
        o.speed = tc.value("speed", "");
    }

    Board initial(w, w);
    if (d.contains("initial_state") && d["initial_state"].is_object()) {
        for (int i = 0; i < 2; i++) {
            std::string stones = d["initial_state"].value(i == 0 ? "black" : "white", "");
            std::vector<bool> mask;
            parseCoords(stones, w, mask);
            for (int k = 0; k < w * w; k++)
                if (mask[k])
                    initial.setStone(Location::getLoc(k % w, k / w, w), i == 0 ? P_BLACK : P_WHITE);
        }
    }
    Player first = d.value("initial_player", "black") == "white" ? P_WHITE : P_BLACK;
    bool wasLoaded = o.loaded;
    m_game.startFrom(s, initial, first);
    o.next = first;
    o.serverMoves = 0;
    if (d.contains("moves") && d["moves"].is_array())
        for (const json& mv : d["moves"]) {
            json ev = {{"move_number", o.serverMoves + 1}, {"move", mv}};
            ogsApplyMove(ev);
        }
    o.phase = d.value("phase", "play");
    o.movePending = false;
    if (d.contains("clock") && d["clock"].is_object())
        ogsSetClock(d["clock"]);
    o.removed = d.contains("removed") && d["removed"].is_string() ? d["removed"].get<std::string>() : "";
    if (o.phase == "stone removal")
        ogsSetRemoved(o.removed);

    if (o.phase == "finished") {
        int64_t winnerId = d.value("winner", (int64_t)0);
        Player winner = winnerId == o.blackId ? P_BLACK : (winnerId == o.whiteId ? P_WHITE : C_EMPTY);
        std::string outcome = d.value("outcome", "");
        const char* wc = winner == P_BLACK ? "B" : "W";
        bool blackWon = winner == P_BLACK;
        ScoreResult sc;
        EndReason reason = EndReason::Other;
        std::string text;
        if (outcome.find("esign") != std::string::npos) {
            reason = EndReason::Resign;
            text = strf("%s+R", wc);
            o.outcome = blackWon ? L("Black wins by resignation", "Noir gagne par abandon")
                                 : L("White wins by resignation", "Blanc gagne par abandon");
        } else if (outcome.find("point") != std::string::npos) {
            reason = EndReason::Score;
            std::vector<bool> dead;
            parseCoords(o.removed, w, dead);
            sc = m_game.scoreWithDead(dead);
            // Les chiffres officiels sont ceux du serveur
            if (d.contains("score") && d["score"].is_object()) {
                const json& js = d["score"];
                if (js.contains("black") && js["black"].is_object()) {
                    sc.blackScore = js["black"].value("total", sc.blackScore);
                    sc.blackTerritory = js["black"].value("territory", sc.blackTerritory);
                    sc.blackPrisoners = js["black"].value("prisoners", sc.blackPrisoners);
                }
                if (js.contains("white") && js["white"].is_object()) {
                    sc.whiteScore = js["white"].value("total", sc.whiteScore);
                    sc.whiteTerritory = js["white"].value("territory", sc.whiteTerritory);
                    sc.whitePrisoners = js["white"].value("prisoners", sc.whitePrisoners);
                }
            }
            double pts = atof(outcome.c_str());
            text = strf("%s+%g", wc, pts);
            o.outcome = strf("%s %g %s", blackWon ? L("Black wins by", "Noir gagne de") : L("White wins by", "Blanc gagne de"),
                             pts, L("points", "points"));
        } else if (outcome.find("imeout") != std::string::npos) {
            text = strf("%s+T", wc);
            o.outcome = blackWon ? L("Black wins on time", "Noir gagne au temps") : L("White wins on time", "Blanc gagne au temps");
        } else if (winner == C_EMPTY) {
            text = "Void";
            o.outcome = strf("%s (%s)", L("Game ended", "Partie terminée"), outcome.c_str());
        } else {
            text = strf("%s+F", wc);
            o.outcome = strf("%s (%s)", blackWon ? L("Black wins", "Noir gagne") : L("White wins", "Blanc gagne"),
                             outcome.c_str());
        }
        m_game.setExternalResult(reason, winner, sc, text);
        m_endFocus = 0;
    }

    o.loaded = true;
    if (!wasLoaded) {
        m_cursor = Location::getLoc(w / 2, w / 2, w);
        m_cursorVisible = false;
    }
    m_dirty = true;
}

void App::ogsApplyMove(const json& d)
{
    Online& o = m_online;
    int number = d.value("move_number", 0);
    if (number <= o.serverMoves || !d.contains("move") || !d["move"].is_array() || d["move"].size() < 2)
        return;  // déjà reçu (reconnexion)
    o.serverMoves = number;
    o.movePending = false;
    int n = m_game.size();
    int x = d["move"][0].get<int>(), y = d["move"][1].get<int>();
    bool inHandicap = o.freeHandicap && m_game.moveCount() < o.handicap;
    if (x < 0 && inHandicap)
        return;  // ancienne passe implicite pendant le handicap : ignorée par OGS
    Player pla = inHandicap ? P_BLACK : o.next;
    Loc loc = (x < 0 || y < 0 || x >= n || y >= n) ? Board::PASS_LOC : Location::getLoc(x, y, n);
    m_game.appendForced(loc, pla);
    bool stillHandicap = o.freeHandicap && m_game.moveCount() < o.handicap;
    o.next = stillHandicap ? P_BLACK : getOpp(pla);
    if (o.loaded && loc == Board::PASS_LOC)
        toast(pla == P_BLACK ? L("Black passes", "Noir passe") : L("White passes", "Blanc passe"));
    m_dirty = true;
}

void App::ogsSetClock(const json& c)
{
    Online& o = m_online;
    o.hasClock = true;
    o.currentPlayer = c.value("current_player", (int64_t)0);
    o.lastMove = c.value("last_move", (int64_t)0);
    o.paused = c.contains("pause") && c["pause"].is_object() && c["pause"].value("paused", false);
    for (int i = 0; i < 2; i++) {
        const char* key = i == 0 ? "black_time" : "white_time";
        OgsClockSide& side = o.clock[i];
        side = OgsClockSide();
        if (!c.contains(key))
            continue;
        const json& t = c[key];
        if (t.is_number()) {
            // Temps absolu : instant (ms) où la pendule tombe
            side.thinking = (t.get<double>() - (double)o.lastMove) / 1000.0;
            continue;
        }
        if (!t.is_object())
            continue;
        side.thinking = t.value("thinking_time", 0.0);
        side.periods = t.value("periods", 0);
        side.periodTime = t.value("period_time", 0.0);
        side.movesLeft = t.value("moves_left", 0);
        side.blockTime = t.value("block_time", 0.0);
    }
}

void App::ogsSetRemoved(const std::string& all)
{
    m_online.removed = all;
    std::vector<bool> dead;
    parseCoords(all, m_game.size(), dead);
    m_online.removal = m_game.scoreWithDead(dead);
    m_dirty = true;
}

void App::ogsToggleGroup(Loc loc)
{
    Online& o = m_online;
    if (o.phase != "stone removal" || o.me == C_EMPTY)
        return;
    int n = m_game.size();
    const Board& b = m_game.board();
    Color c = b.colors[loc];
    if (c != C_BLACK && c != C_WHITE)
        return;
    // Toute la chaîne touchée
    std::vector<bool> seen(n * n, false);
    std::vector<Loc> stack = {loc};
    std::string stones;
    seen[Location::getY(loc, n) * n + Location::getX(loc, n)] = true;
    while (!stack.empty()) {
        Loc l = stack.back();
        stack.pop_back();
        int x = Location::getX(l, n), y = Location::getY(l, n);
        stones += (char)('a' + x);
        stones += (char)('a' + y);
        const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (auto& p : nb) {
            if (p[0] < 0 || p[0] >= n || p[1] < 0 || p[1] >= n || seen[p[1] * n + p[0]])
                continue;
            Loc m = Location::getLoc(p[0], p[1], n);
            if (b.colors[m] == c) {
                seen[p[1] * n + p[0]] = true;
                stack.push_back(m);
            }
        }
    }
    bool removed = o.removal.valid && o.removal.dead[Location::getY(loc, n) * n + Location::getX(loc, n)];
    m_ogs.removedSet(o.gameId, !removed, stones);
}

bool App::ogsMyTurn() const
{
    return m_online.me != C_EMPTY && m_online.next == m_online.me;
}

std::string App::ogsPlayerName(Player p) const
{
    const std::string& name = p == P_BLACK ? m_online.blackName : m_online.whiteName;
    std::string out = name.empty() ? std::string(p == P_BLACK ? L("Black", "Noir") : L("White", "Blanc")) : name;
    if (m_online.me == p)
        out += strf(" · %s", L("you", "toi"));
    return out;
}

std::string App::ogsClockText(Player p) const
{
    const Online& o = m_online;
    const OgsClockSide& cs = o.clock[p == P_BLACK ? 0 : 1];
    int64_t pid = p == P_BLACK ? o.blackId : o.whiteId;
    double elapsed = 0;
    if (o.phase == "play" && !o.paused && o.currentPlayer == pid && o.lastMove > 0)
        elapsed = std::max<double>(0, (m_ogs.serverNowMs() - o.lastMove) / 1000.0);
    double main = cs.thinking - elapsed;
    if (o.system == "byoyomi") {
        if (main > 0)
            return cs.periods ? strf("%s +%d×%s", fmtDuration(main).c_str(), cs.periods, fmtDuration(cs.periodTime).c_str())
                              : fmtDuration(main);
        if (cs.periodTime <= 0)
            return "0:00";
        double over = -main;
        int used = (int)(over / cs.periodTime);
        int left = cs.periods - used;
        if (left <= 0)
            return "0:00";
        return strf("%d× %s", left, fmtDuration(cs.periodTime - (over - used * cs.periodTime)).c_str());
    }
    if (o.system == "canadian" && main <= 0)
        return strf("%s / %d", fmtDuration(cs.blockTime + main).c_str(), cs.movesLeft);
    return fmtDuration(main);
}

// ------------------------------------------------------------------ partie en ligne : entrées et panneau

enum OnlineAction { OA_PASS, OA_ACCEPT, OA_RESUME, OA_MENU };

static std::vector<std::pair<Rect, int>> onlineButtons(bool removal, bool player)
{
    int x = PANEL_X, w = (PANEL_W - 16) / 2, h = 60, y = 470;
    if (!player)
        return {{{x, y, w, h}, OA_MENU}};
    if (removal)
        return {{{x, y, w, h}, OA_ACCEPT}, {{x + w + 16, y, w, h}, OA_RESUME}, {{x, y + h + 14, w, h}, OA_MENU}};
    return {{{x, y, w, h}, OA_PASS}, {{x + w + 16, y, w, h}, OA_MENU}};
}

void App::updateOnline(const Input& in)
{
    Online& o = m_online;
    bool removal = o.phase == "stone removal";
    bool player = o.me != C_EMPTY;
    int action = -1;
    if (in.pressed(HidNpadButton_Plus))
        action = OA_MENU;
    if (player && !removal && in.pressed(HidNpadButton_Y))
        action = OA_PASS;
    if (player && removal && in.pressed(HidNpadButton_X))
        action = OA_ACCEPT;
    if (player && removal && in.pressed(HidNpadButton_Y))
        action = OA_RESUME;
    for (auto& b : onlineButtons(removal, player))
        if (b.first.tapped(in))
            action = b.second;
    if (in.pressed(HidNpadButton_X) && !removal)
        toast(L("Analysis is not allowed during an online game", "Analyse interdite pendant une partie en ligne"));

    switch (action) {
    case OA_PASS: doPass(); break;
    case OA_ACCEPT:
        if (o.loaded && removal) {
            m_ogs.removedAccept(o.gameId, o.removed);
            o.iAccepted = true;
        }
        break;
    case OA_RESUME:
        if (o.loaded && removal)
            m_ogs.removedReject(o.gameId);
        break;
    case OA_MENU:
        m_paused = true;
        m_pauseFocus = 0;
        o.confirmResign = false;
        break;
    default: break;
    }
}

void App::drawOnlinePanel()
{
    const Online& o = m_online;
    int ay = 228;
    gfx_round_rect(PANEL_X, ay, PANEL_W, 132, 16, PANEL);
    // Ligne 1 : type de partie
    std::string kind = strf("%dx%d", m_game.size(), m_game.size());
    if (!o.speed.empty())
        kind += strf("  ·  %s", speedName(o.speed));
    kind += strf("  ·  %s", o.ranked ? L("ranked", "classée") : L("friendly", "amicale"));
    if (o.handicap)
        kind += strf("  ·  %s %d", L("handicap", "handicap"), o.handicap);
    gfx_text_fit(PANEL_X + 20, ay + 16, 20, DIM, kind.c_str(), PANEL_W - 40);

    // Ligne 2 : état
    std::string status;
    GfxColor sc = TEXT;
    if (!m_ogs.socketConnected()) {
        status = L("Connection lost, reconnecting…", "Connexion perdue, reconnexion…");
        sc = DANGER;
    } else if (!o.loaded) {
        status = L("Connecting to the game…", "Connexion à la partie…");
    } else if (o.phase == "stone removal") {
        status = o.me == C_EMPTY ? L("Players are marking dead stones", "Les joueurs marquent les pierres mortes")
                                 : L("Tap dead groups, then accept", "Touche les groupes morts, puis accepte");
        sc = WOOD;
    } else if (o.phase == "finished") {
        status = o.outcome;
    } else if (o.me == C_EMPTY) {
        status = L("You are watching this game", "Tu regardes cette partie");
    } else if (o.movePending) {
        status = L("Sending your move…", "Envoi de ton coup…");
    } else if (ogsMyTurn()) {
        status = o.freeHandicap && m_game.moveCount() < o.handicap ? L("Place your handicap stones", "Pose tes pierres de handicap")
                                                                    : L("Your turn", "À toi de jouer");
        sc = ACCENT;
    } else {
        status = L("Waiting for your opponent…", "En attente de ton adversaire…");
    }
    gfx_text_fit(PANEL_X + 20, ay + 50, 26, sc, status.c_str(), PANEL_W - 40);

    // Ligne 3 : décompte en cours ou rappel de la règle
    std::string l3;
    if (o.phase == "stone removal" && o.removal.valid) {
        l3 = strf("%s %g  ·  %s %g", L("Black", "Noir"), o.removal.blackScore, L("White", "Blanc"), o.removal.whiteScore);
        if (o.iAccepted)
            l3 += strf("  ·  %s", L("you accepted", "tu as accepté"));
        if (o.oppAccepted)
            l3 += strf("  ·  %s", L("opponent accepted", "adversaire d'accord"));
    } else {
        l3 = L("KataGo is off during online games (OGS rules)", "KataGo est coupé en partie en ligne (règle d'OGS)");
    }
    gfx_text_fit(PANEL_X + 20, ay + 96, 20, DIM, l3.c_str(), PANEL_W - 40);

    // Coup courant
    std::string info = strf("%s %d", L("Move", "Coup"), m_game.cursor());
    Loc last = m_game.lastMove();
    if (last != Board::NULL_LOC) {
        const char* who = m_game.lastMovePlayer() == P_BLACK ? L("Black", "Noir") : L("White", "Blanc");
        if (last == Board::PASS_LOC)
            info += strf("   ·   %s %s", who, L("passed", "a passé"));
        else
            info += strf("   ·   %s %s", who, Game::coordString(last, m_game.size()).c_str());
    }
    gfx_text(PANEL_X + 4, 386, 24, TEXT, info.c_str());

    bool removal = o.phase == "stone removal";
    bool player = o.me != C_EMPTY;
    for (auto& b : onlineButtons(removal, player)) {
        switch (b.second) {
        case OA_PASS: drawButton(b.first, L("Pass", "Passer"), "Y", false, humanCanPlay()); break;
        case OA_ACCEPT: drawButton(b.first, L("Accept score", "Accepter"), "X", false, !o.iAccepted, o.iAccepted); break;
        case OA_RESUME: drawButton(b.first, L("Resume play", "Reprendre"), "Y", false); break;
        case OA_MENU: drawButton(b.first, L("Menu", "Menu"), "+", false); break;
        }
    }
    if (removal)
        drawHints({{"A", L("Mark dead", "Marquer mort")}, {"X", L("Accept", "Accepter")}, {"Y", L("Resume", "Reprendre")}});
    else
        drawHints({{"A", L("Play", "Jouer")}, {"Y", L("Pass", "Passer")}, {"+", L("Menu", "Menu")}});
}

// ------------------------------------------------------------------ écran OGS

static const int OGS_LIST_X = 64, OGS_LIST_Y = 170, OGS_ROW = 78, OGS_LIST_H = 468, OGS_LIST_W = 620;
static const int OGS_SIDE_X = 720, OGS_SIDE_W = 496;

enum OgsSideItem { OS_SIZE, OS_SPEED, OS_SEARCH, OS_REFRESH, OS_LOGOUT, OS_COUNT };

static Rect ogsTabRect(int tab)
{
    return {OGS_LIST_X + tab * 250, 118, 238, 42};
}

static Rect ogsSideRect(int item)
{
    int y = 214 + item * 74;
    if (item >= OS_REFRESH)
        y += 30;
    return {OGS_SIDE_X, y, OGS_SIDE_W, 62};
}

// Connexion : boutons
enum OgsLoginItem { OL_PASSWORD, OL_BACK, OL_COUNT };

static Rect ogsLoginRect(int item)
{
    return {120, 360 + item * 76, 560, 62};
}

void App::ogsSignInPassword()
{
    std::string user = g_settings.ogsUser, pass;
    if (!askText(L("online-go.com username", "Identifiant online-go.com"), false, user))
        return;
    if (!askText(L("online-go.com password", "Mot de passe online-go.com"), true, pass))
        return;
    g_settings.ogsUser = user;
    settings_save();
    m_ogs.loginPassword(user, pass);
}

void App::updateOgs(const Input& in)
{
    OgsState st = m_ogs.state();
    if (st != OgsState::Online) {
        if (st == OgsState::LoggingIn) {
            if (in.pressed(HidNpadButton_B) || Rect{120, 600, 360, 62}.tapped(in))
                m_ogs.cancelLogin();
            return;
        }
        if (in.repeated(HidNpadButton_AnyUp))
            m_ogsFocus = (m_ogsFocus + OL_COUNT - 1) % OL_COUNT;
        if (in.repeated(HidNpadButton_AnyDown))
            m_ogsFocus = (m_ogsFocus + 1) % OL_COUNT;
        m_ogsFocus = std::clamp(m_ogsFocus, 0, OL_COUNT - 1);
        int chosen = in.pressed(HidNpadButton_A) ? m_ogsFocus : -1;
        for (int i = 0; i < OL_COUNT; i++)
            if (ogsLoginRect(i).tapped(in))
                chosen = m_ogsFocus = i;

        if (in.pressed(HidNpadButton_B))
            chosen = OL_BACK;
        switch (chosen) {
        case OL_PASSWORD:
            if (m_ogs.available())
                ogsSignInPassword();
            break;
        case OL_BACK: m_screen = Screen::Menu; break;
        default: break;
        }
        return;
    }

    // Recherche en cours : seule l'annulation est possible
    if (m_ogs.searching()) {
        if (in.pressed(HidNpadButton_B) || Rect{390, 440, 500, 62}.tapped(in))
            m_ogs.cancelMatch();
        return;
    }

    // Onglets : parties en cours / historique (L/R ou toucher)
    int tab = m_ogsTab;
    if (in.pressed(HidNpadButton_L) || in.pressed(HidNpadButton_R))
        tab = 1 - tab;
    for (int t = 0; t < 2; t++)
        if (ogsTabRect(t).tapped(in))
            tab = t;
    if (tab != m_ogsTab) {
        m_ogsTab = tab;
        m_ogsFocus = m_ogsListFocus = 0;
        m_ogsScroll = 0;
        if (tab == 1)
            m_ogs.refreshGames();
    }

    auto games = m_ogsTab == 0 ? m_ogs.games() : m_ogs.history();
    int nGames = (int)games.size();
    int count = nGames + OS_COUNT;  // parties puis colonne de droite
    if (m_ogsFocus >= count)
        m_ogsFocus = count - 1;
    if (in.repeated(HidNpadButton_AnyUp))
        m_ogsFocus = (m_ogsFocus + count - 1) % count;
    if (in.repeated(HidNpadButton_AnyDown))
        m_ogsFocus = (m_ogsFocus + 1) % count;
    // Gauche/droite : passer de la liste à la colonne (sauf sur une ligne réglable)
    bool onValue = m_ogsFocus == nGames + OS_SIZE || m_ogsFocus == nGames + OS_SPEED;
    if (!onValue && in.pressed(HidNpadButton_AnyRight) && m_ogsFocus < nGames)
        m_ogsFocus = nGames + OS_SEARCH;
    if (!onValue && in.pressed(HidNpadButton_AnyLeft) && m_ogsFocus >= nGames && nGames > 0)
        m_ogsFocus = std::min(m_ogsListFocus, nGames - 1);
    if (m_ogsFocus < nGames)
        m_ogsListFocus = m_ogsFocus;

    // Défilement de la liste
    float maxScroll = std::max(0, nGames * OGS_ROW - OGS_LIST_H);
    if (m_ogsFocus < nGames && (in.down & (HidNpadButton_AnyUp | HidNpadButton_AnyDown))) {
        float top = m_ogsFocus * OGS_ROW, bottom = top + OGS_ROW;
        if (top < m_ogsScroll)
            m_ogsScroll = top;
        if (bottom > m_ogsScroll + OGS_LIST_H)
            m_ogsScroll = bottom - OGS_LIST_H;
    }
    if (in.touching && in.touchIn(OGS_LIST_X, OGS_LIST_Y, OGS_LIST_W, OGS_LIST_H))
        m_ogsScroll -= in.dragY;
    m_ogsScroll = std::clamp(m_ogsScroll, 0.0f, maxScroll);

    int chosen = in.pressed(HidNpadButton_A) ? m_ogsFocus : -1;
    if (in.tap && in.x >= OGS_LIST_X && in.x < OGS_LIST_X + OGS_LIST_W && in.y >= OGS_LIST_Y &&
        in.y < OGS_LIST_Y + OGS_LIST_H) {
        int i = (int)((in.y - OGS_LIST_Y + m_ogsScroll) / OGS_ROW);
        if (i >= 0 && i < nGames)
            chosen = m_ogsFocus = i;
    }
    for (int k = 0; k < OS_COUNT; k++)
        if (ogsSideRect(k).tapped(in) && k != OS_SIZE && k != OS_SPEED)
            chosen = m_ogsFocus = nGames + k;

    // Lignes réglables (taille, cadence)
    Settings& s = g_settings;
    for (int k = OS_SIZE; k <= OS_SPEED; k++) {
        Rect r = ogsSideRect(k);
        bool focused = m_ogsFocus == nGames + k;
        int d = 0;
        if (Rect{r.x, r.y, 90, r.h}.tapped(in))
            d = -1;
        else if (Rect{r.x + r.w - 90, r.y, 90, r.h}.tapped(in))
            d = 1;
        else if (focused && in.repeated(HidNpadButton_AnyLeft))
            d = -1;
        else if (focused && in.repeated(HidNpadButton_AnyRight))
            d = 1;
        if (!d)
            continue;
        m_ogsFocus = nGames + k;
        if (k == OS_SIZE)
            s.ogsSize = (s.ogsSize + d + 3) % 3;
        else
            s.ogsSpeed = (s.ogsSpeed + d + 4) % 4;
        settings_save();
    }

    if (in.pressed(HidNpadButton_Y))
        chosen = nGames + OS_REFRESH;
    if (in.pressed(HidNpadButton_Minus))
        chosen = nGames + OS_LOGOUT;
    if (in.pressed(HidNpadButton_B)) {
        m_screen = Screen::Menu;
        return;
    }

    if (chosen < 0)
        return;
    if (chosen < nGames) {
        if (m_ogsTab == 0) {
            openOnlineGame(games[chosen].id);
        } else {
            m_ogs.downloadSgf(games[chosen].id);
            toast(L("Downloading the game…", "Téléchargement de la partie…"));
        }
        return;
    }
    switch (chosen - nGames) {
    case OS_SEARCH:
        if (m_ogs.socketConnected())
            m_ogs.findMatch(SIZE_KEYS[std::clamp(s.ogsSize, 0, 2)], SPEED_KEYS[std::clamp(s.ogsSpeed, 0, 3)]);
        else
            toast(L("Not connected to OGS yet", "Pas encore connecté à OGS"));
        break;
    case OS_REFRESH: m_ogs.refreshGames(); break;
    case OS_LOGOUT:
        m_ogs.logout();
        m_ogsFocus = 0;
        break;
    default: break;
    }
}

void App::drawOgs()
{
    OgsState st = m_ogs.state();
    if (st == OgsState::LoggingIn) {
        drawTitle(L("Play online on OGS", "Jouer en ligne sur OGS"), "online-go.com");
        gfx_text(120, 300, 30, TEXT, L("Signing in…", "Connexion…"));
        drawButton({120, 600, 360, 62}, L("Cancel", "Annuler"), "B", false);
        drawHints({{"B", L("Cancel", "Annuler")}});
        return;
    }
    if (st == OgsState::LoggedOut) {
        drawTitle(L("Play online on OGS", "Jouer en ligne sur OGS"), "online-go.com");
        const char* lines[] = {
            L("Play ranked and friendly games against players worldwide.",
              "Joue des parties classées ou amicales contre le monde entier."),
            L("While a game is in progress, KataGo is fully switched off (OGS rules):",
              "Pendant une partie, KataGo est entièrement coupé (règle d'OGS) :"),
            L("no analysis, no hints, no score estimate. Review it once it is over.",
              "ni analyse, ni indice, ni estimation. Revois-la une fois finie."),
        };
        int y = 150;
        for (const char* l : lines) {
            gfx_text_fit(124, y, 24, TEXT, l, 1040);
            y += 40;
        }
        std::string err = m_ogs.error();
        if (!err.empty())
            gfx_text_fit(124, 300, 22, DANGER, err.c_str(), 1040);
        drawButton(ogsLoginRect(OL_PASSWORD), L("Sign in to online-go.com", "Se connecter à online-go.com"), "A",
                   m_ogsFocus == OL_PASSWORD, m_ogs.available());
        drawButton(ogsLoginRect(OL_BACK), L("Back", "Retour"), "B", m_ogsFocus == OL_BACK);
        if (!m_ogs.available())
            gfx_text_fit(720, 380, 18, DIM,
                         L("Missing OGS app ID: set ogs_client_id in /switch/nxgo/config.txt",
                           "ID d'appli OGS manquant : ogs_client_id dans /switch/nxgo/config.txt"),
                         500);
        gfx_text_fit(720, 450, 18, DIM,
                     L("The password is only used to get a sign-in token; it is never stored.",
                       "Le mot de passe sert juste à obtenir un jeton de connexion ; il n'est jamais enregistré."),
                     500);
        drawHints({{"A", L("Select", "Choisir")}, {"B", L("Back", "Retour")}});
        return;
    }

    // Connecté
    std::string who = strf("%s %s", L("Signed in as", "Connecté en tant que"), m_ogs.username().c_str());
    drawTitle(L("Online (OGS)", "En ligne (OGS)"), who.c_str());
    bool live = m_ogs.socketConnected();
    gfx_disc(1180, 64, 9, live ? GOOD : DANGER);
    const char* conn = live ? L("connected", "connecté") : L("connecting…", "connexion…");
    gfx_text(1160 - gfx_text_width(20, conn), 52, 20, DIM, conn);

    auto games = m_ogsTab == 0 ? m_ogs.games() : m_ogs.history();
    int nGames = (int)games.size();
    for (int t = 0; t < 2; t++) {
        Rect r = ogsTabRect(t);
        bool on = m_ogsTab == t;
        gfx_round_rect(r.x, r.y, r.w, r.h, 21, on ? RGB(40, 92, 150) : PANEL);
        std::string label = t == 0 ? strf("%s (%d)", L("In progress", "En cours"), (int)m_ogs.games().size())
                                   : std::string(L("History", "Historique"));
        gfx_text(r.x + (r.w - gfx_text_width(22, label.c_str())) / 2, r.y + 9, 22, on ? TEXT : DIM, label.c_str());
    }
    if (games.empty())
        gfx_text_fit(OGS_LIST_X, OGS_LIST_Y + 20, 22, DIM,
                     m_ogs.gamesLoading() ? L("Loading…", "Chargement…")
                     : m_ogsTab == 0      ? L("No game in progress. Find an opponent on the right!",
                                              "Aucune partie en cours. Trouve un adversaire à droite !")
                                          : L("No finished game yet.", "Aucune partie terminée pour l'instant."),
                     OGS_LIST_W);
    gfx_set_clip(0, OGS_LIST_Y, SCREEN_W, OGS_LIST_H);
    for (int i = 0; i < nGames; i++) {
        int y = OGS_LIST_Y + i * OGS_ROW - (int)m_ogsScroll;
        if (y + OGS_ROW < OGS_LIST_Y || y > OGS_LIST_Y + OGS_LIST_H)
            continue;
        const OgsGameInfo& g = games[i];
        Rect r = {OGS_LIST_X, y + 4, OGS_LIST_W, OGS_ROW - 8};
        bool f = m_ogsFocus == i;
        gfx_round_rect(r.x, r.y, r.w, r.h, 14, f ? PANEL_HI : PANEL);
        if (f)
            gfx_round_rect(r.x, r.y, 6, r.h, 3, ACCENT);
        bool iAmBlack = g.blackId == m_ogs.userId();
        std::string opp = iAmBlack ? g.white : g.black;
        BoardView::drawStoneIcon(r.x + 36, r.y + r.h / 2, 16, iAmBlack ? P_BLACK : P_WHITE);
        gfx_text_fit(r.x + 66, r.y + 10, 24, TEXT, strf("%s %s", L("vs", "contre"), opp.c_str()).c_str(), r.w - 240);
        std::string sub = strf("%dx%d", g.width, g.height);
        if (!g.speed.empty())
            sub += strf("  ·  %s", speedName(g.speed));
        if (m_ogsTab == 0)
            sub += strf("  ·  %s %d", L("move", "coup"), g.moves);
        else if (!g.date.empty())
            sub += strf("  ·  %s", g.date.c_str());
        gfx_text_fit(r.x + 66, r.y + 42, 18, DIM, sub.c_str(), r.w - 240);
        if (m_ogsTab == 1) {
            // Résultat : vert gagné, rouge perdu
            const char* res = g.result.c_str();
            int w = gfx_text_width(20, res) + 28;
            GfxColor bg = g.won == 1 ? RGB(30, 70, 40) : (g.won == 0 ? RGB(80, 34, 32) : PANEL_HI);
            gfx_round_rect(r.x + r.w - w - 16, r.y + 20, w, 32, 16, bg);
            gfx_text(r.x + r.w - w - 2, r.y + 24, 20, g.won == 1 ? GOOD : (g.won == 0 ? DANGER : TEXT), res);
        } else if (g.myTurn) {
            const char* t = L("Your turn", "À toi");
            int w = gfx_text_width(18, t) + 24;
            gfx_round_rect(r.x + r.w - w - 16, r.y + 22, w, 28, 14, RGB(30, 70, 120));
            gfx_text(r.x + r.w - w - 4, r.y + 26, 18, ACCENT, t);
        }
    }
    gfx_reset_clip();

    // Colonne de droite : nouvelle partie
    const Settings& s = g_settings;
    gfx_text(OGS_SIDE_X, 130, 24, DIM, L("New game", "Nouvelle partie"));
    gfx_text_fit(OGS_SIDE_X, 168, 18, DIM,
                 L("Automatic pairing with a player of your level.", "Appariement automatique avec un joueur de ton niveau."),
                 OGS_SIDE_W);
    for (int k = OS_SIZE; k <= OS_SPEED; k++) {
        Rect r = ogsSideRect(k);
        bool f = m_ogsFocus == nGames + k;
        gfx_round_rect(r.x, r.y, r.w, r.h, 14, f ? PANEL_HI : PANEL);
        if (f)
            gfx_round_rect(r.x, r.y, 6, r.h, 3, ACCENT);
        int cy = r.y + r.h / 2;
        GfxColor ac = f ? ACCENT : DIM;
        gfx_triangle(r.x + 50, cy - 12, r.x + 50, cy + 12, r.x + 32, cy, ac);
        gfx_triangle(r.x + r.w - 50, cy - 12, r.x + r.w - 50, cy + 12, r.x + r.w - 32, cy, ac);
        std::string v = k == OS_SIZE ? SIZE_KEYS[std::clamp(s.ogsSize, 0, 2)] : speedName(SPEED_KEYS[std::clamp(s.ogsSpeed, 0, 3)]);
        gfx_text(r.x + (r.w - gfx_text_width(24, v.c_str())) / 2, r.y + 17, 24, TEXT, v.c_str());
    }
    drawButton(ogsSideRect(OS_SEARCH), L("Find an opponent", "Trouver un adversaire"), nullptr,
               m_ogsFocus == nGames + OS_SEARCH, live);
    drawButton(ogsSideRect(OS_REFRESH), L("Refresh", "Actualiser"), "Y", m_ogsFocus == nGames + OS_REFRESH);
    drawButton(ogsSideRect(OS_LOGOUT), L("Sign out", "Se déconnecter"), "-", m_ogsFocus == nGames + OS_LOGOUT);
    drawHints({{"A", m_ogsTab == 0 ? L("Open", "Ouvrir") : L("Analyse", "Analyser")},
               {"L/R", L("Tab", "Onglet")},
               {"Y", L("Refresh", "Actualiser")},
               {"B", L("Back", "Retour")}});

    if (m_ogs.searching()) {
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 160));
        gfx_round_rect(360, 220, 560, 300, 24, RGB(30, 34, 43));
        gfx_text(390, 250, 30, TEXT, L("Looking for an opponent…", "Recherche d'un adversaire…"));
        std::string what = strf("%s  ·  %s", SIZE_KEYS[std::clamp(s.ogsSize, 0, 2)],
                                speedName(SPEED_KEYS[std::clamp(s.ogsSpeed, 0, 3)]));
        gfx_text(390, 300, 24, DIM, what.c_str());
        int dots = (int)(nowMs() / 400 % 4);
        gfx_text(390, 346, 24, ACCENT, std::string(dots + 1, '.').c_str());
        drawButton({390, 440, 500, 62}, L("Cancel", "Annuler"), "B", true);
    }
}
