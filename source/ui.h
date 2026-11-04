#pragma once

#include "board_view.h"
#include "engine.h"
#include "game.h"
#include "input.h"

// Écrans de l'appli et boucle principale.
class App {
public:
    explicit App(Engine& engine);
    // Retourne faux quand l'utilisateur quitte.
    bool frame();

private:
    enum class Screen { Menu, Setup, Game, SgfList, Settings, Ogs };
    enum class Mode { VsAI, TwoPlayers, Sandbox };

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

    std::string m_toast;
    u64 m_toastUntil = 0;
};
