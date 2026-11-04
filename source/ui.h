#pragma once

#include "board_view.h"
#include "engine.h"
#include "game.h"
#include "input.h"
#include "ogs.h"

// Écrans de l'appli et boucle principale.
class App {
public:
    explicit App(Engine& engine);
    ~App();
    // Retourne faux quand l'utilisateur quitte.
    bool frame();

private:
    enum class Screen { Menu, Setup, Game, SgfList, Settings, Ogs };
    enum class Mode { VsAI, TwoPlayers, Sandbox, Online };

    // Écrans : update() traite les entrées, draw() dessine.
    void updateMenu(const Input& in);
    void drawMenu();
    void updateSetup(const Input& in);
    void drawSetup();
    void updateGame(const Input& in);
    void drawGame();
    void updateSgfList(const Input& in);
    void drawSgfList();
    void updateSettings(const Input& in);
    void drawSettings();
    void updateOgs(const Input& in);
    void drawOgs();

    // Partie en ligne (ui_ogs.cpp) : le serveur OGS fait foi, KataGo n'intervient pas
    void ogsStep();  // traite les événements reçus (à chaque image)
    void openOnlineGame(int64_t id);
    void ogsLeaveGame();
    void ogsGameEvent(const std::string& kind, const nlohmann::json& d);
    void ogsLoadGamedata(const nlohmann::json& d);
    void ogsApplyMove(const nlohmann::json& mv);
    void ogsSetRemoved(const std::string& all);
    void ogsSetClock(const nlohmann::json& c);
    void ogsToggleGroup(Loc loc);
    bool ogsMyTurn() const;
    std::string ogsClockText(Player p) const;
    std::string ogsPlayerName(Player p) const;
    void updateOnline(const Input& in);
    void drawOnlinePanel();
    void ogsSignInPassword();

    // Partie
    void startGame();
    void openSandbox();
    bool openSgf(const std::string& path);
    void stepEngine();
    void undo();
    void doPass();
    void tryPlay(Loc loc);
    void afterMove();
    void saveSgf();
    bool aiAssistAllowed() const;
    bool humanCanPlay() const;
    void toast(const std::string& msg);

    // Menu pause / fin de partie
    void updatePause(const Input& in);
    void drawPause();
    void drawEndPanel(int x, int y, int w);

    // Courbe de score (revue) : évaluation rapide de chaque coup, affinée par l'analyse
    void stepScoreGraph();
    void drawScoreGraph();

    Engine& m_engine;
    Screen m_screen = Screen::Menu;
    bool m_dirty = true;
    bool m_quit = false;

    // Menu principal
    int m_menuFocus = 0;
    Game m_demo;
    BoardView m_demoView;

    // Nouvelle partie
    Mode m_setupMode = Mode::VsAI;
    int m_setupFocus = 0;

    // Partie en cours
    Mode m_mode = Mode::VsAI;
    Game m_game;
    BoardView m_view;
    Loc m_cursor = Board::NULL_LOC;
    bool m_cursorVisible = false;
    Loc m_touchLoc = Board::NULL_LOC;
    bool m_touchOnBoard = false;
    bool m_analysisOn = false;
    uint64_t m_aiRequested = 0;
    uint64_t m_analysisPos = 0;
    AnalysisSnapshot m_snapshot;
    uint64_t m_snapVersion = 0;
    int m_lowWinrateCount = 0;
    std::vector<float> m_scanLead, m_refined;  // avance de Noir par position
    uint64_t m_scanVersion = 0, m_scanKey = 0;
    std::vector<Move> m_scanMoves;  // coups de la dernière courbe (pour garder le début commun)
    bool m_paused = false;
    int m_pauseFocus = 0;
    int m_endFocus = 0;
    bool m_wasThinking = false;
    bool m_wasLoading = true;

    // Listes (SGF) et réglages
    std::vector<std::string> m_files;
    int m_listFocus = 0;
    float m_listScroll = 0;
    int m_settingsFocus = 0;

    // OGS
    Ogs m_ogs;
    int m_ogsFocus = 0;
    int m_ogsListFocus = 0;
    int m_ogsTab = 0;  // 0 parties en cours, 1 historique
    bool m_backToOgs = false;  // revue ouverte depuis OGS : le menu y ramène
    float m_ogsScroll = 0;
    struct OgsClockSide {
        double thinking = 0, periodTime = 0, blockTime = 0;
        int periods = 0, movesLeft = 0;
    };
    struct Online {
        int64_t gameId = 0;
        bool loaded = false;
        Player me = C_EMPTY;  // C_EMPTY : simple spectateur
        std::string phase, system, speed, outcome;
        std::string blackName, whiteName;
        int64_t blackId = 0, whiteId = 0;
        bool ranked = false;
        int handicap = 0;
        bool freeHandicap = false;
        int serverMoves = 0;          // coups reçus du serveur (y compris passes de handicap)
        Player next = P_BLACK;        // au trait selon OGS (handicap libre : Noir plusieurs fois)
        bool movePending = false;     // coup envoyé, en attente de l'écho du serveur
        // Pendule
        bool hasClock = false, paused = false;
        int64_t currentPlayer = 0, lastMove = 0;
        OgsClockSide clock[2];        // 0 Noir, 1 Blanc
        // Retrait des pierres mortes
        std::string removed;
        ScoreResult removal;
        bool oppAccepted = false, iAccepted = false;
        bool confirmResign = false;
    } m_online;
    u64 m_lastClockDraw = 0;

    std::string m_toast;
    u64 m_toastUntil = 0;
};
