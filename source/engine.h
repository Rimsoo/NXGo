#pragma once

// Pont entre l'interface et KataGo.
//
// Les réseaux se chargent dans un thread à part au démarrage. Ensuite, deux usages :
//  - jouer : requestMove() lance une réflexion, pollMove() rend le coup quand il est prêt ;
//  - analyser : startAnalysis() lance une recherche continue, getAnalysis() rend le
//    dernier état (taux de victoire, meilleurs coups, territoires).
// Une seule recherche tourne à la fois : le processeur de la Switch n'a que 3 cœurs.
//
// Toutes les méthodes sont à appeler depuis le thread de l'interface.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "game/board.h"
#include "game/boardhistory.h"
#include "game/rules.h"

class AsyncBot;
class NNEvaluator;
class Logger;
class ConfigParser;

enum class NetId { B6, B10, B15, Count };

struct AnalysisMove {
    Loc loc;
    int64_t visits;
    double winrate;  // pour le joueur au trait, 0..1
    double lead;     // avance en points pour le joueur au trait
    double prior;    // probabilité "intuitive" du réseau
    int order;       // 0 = meilleur coup
    std::vector<Loc> pv;
};

struct AnalysisSnapshot {
    uint64_t positionId = 0;
    int64_t visits = 0;
    double winrateBlack = 0.5;
    double leadBlack = 0;          // points d'avance de Noir (négatif : Blanc mène)
    std::vector<AnalysisMove> moves;
    std::vector<float> ownership;  // par intersection (y * taille + x), +1 Blanc, -1 Noir
    bool valid = false;
};

// Force de l'IA : 1 (débutant) à 10 (le plus fort disponible).
static const int AI_LEVEL_MIN = 1;
static const int AI_LEVEL_MAX = 10;

class Engine {
public:
    Engine();
    ~Engine();

    // Lance le chargement des réseaux trouvés dans romfs:/models et sdmc:/switch/nxgo/models.
    void start();

    bool loading() const { return m_loading; }
    bool ready() const { return !m_loading && m_error.empty(); }
    const std::string& error() const { return m_error; }
    bool hasNet(NetId id) const;
    static const char* netName(NetId id);

    // Réseau utilisé pour l'analyse (le plus fort disponible par défaut).
    void setAnalysisNet(NetId id);
    NetId analysisNet() const { return m_analysisNet; }
    void setAnalysisMaxVisits(int64_t visits) { m_analysisMaxVisits = visits; }

    // --- jouer
    // (la taille du plateau choisit le réseau adapté : 9x9 et 13x13 ont le leur, bien plus rapide)
    void requestMove(const Board& board, const BoardHistory& hist, Player pla, int level, uint64_t positionId);
    bool thinking() const { return m_thinking; }
    // Vrai une seule fois quand le coup demandé est prêt ; positionId identifie la demande.
    bool pollMove(Loc& loc, uint64_t& positionId, double& winrateForMover);
    void cancelMove();

    // --- analyser
    void startAnalysis(const Board& board, const BoardHistory& hist, Player pla, uint64_t positionId);
    void stopAnalysis();
    bool analyzing() const { return m_analyzing; }
    // Copie le dernier état s'il est plus récent que `version` (mis à jour en retour).
    bool getAnalysis(AnalysisSnapshot& out, uint64_t& version);

    // --- courbe de score d'une partie
    // Une évaluation directe du réseau le plus rapide par position (sans recherche) :
    // la courbe se remplit en quelques secondes, dans un thread à part.
    // Position i = après i coups (0 = position de départ).
    void startScan(const Board& initial, Player first, const Rules& rules, const std::vector<Move>& moves);
    void stopScan();
    bool scanning() const { return m_scanning; }
    // Copie l'avance de Noir par position (NaN : pas encore calculée) si elle a changé.
    bool getScan(std::vector<float>& leadBlack, uint64_t& version);

    // Arrête toute recherche en cours (avant de quitter l'appli par exemple).
    void stopAll();

private:
    struct Net;
    void loadThread();
    void loadSlot(Net& n, int id, int slot);
    // Emplacement du réseau pour une taille de plateau : 0 = 9x9, 1 = 13x13, 2 = 19x19 (et autres)
    static int sizeSlot(int boardSize);
    // Réseau prêt pour cette taille (à défaut celui du 19x19, qui accepte toutes les tailles)
    NNEvaluator* evalFor(NetId id, int boardSize, AsyncBot** bot);
    AsyncBot* playBot(int level, int boardSize);
    void publishAnalysis(const void* search, uint64_t positionId, int boardSize, Player pla);

    std::unique_ptr<ConfigParser> m_cfg;
    std::unique_ptr<Logger> m_logger;
    std::unique_ptr<Net> m_nets[(int)NetId::Count];
    std::thread m_loader;
    std::atomic<bool> m_loading{false};
    std::atomic<bool> m_abortLoad{false};
    std::string m_error;

    NetId m_analysisNet = NetId::B10;
    int64_t m_analysisMaxVisits = 400;
    AsyncBot* m_analysisBot = nullptr;
    NNEvaluator* m_analysisBotEval = nullptr;

    std::atomic<bool> m_thinking{false};
    std::atomic<bool> m_moveReady{false};
    Loc m_moveLoc = Board::NULL_LOC;
    double m_moveWinrate = 0.5;
    uint64_t m_movePositionId = 0;
    AsyncBot* m_moveBot = nullptr;

    std::thread m_scanThread;
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_scanAbort{false};
    std::vector<float> m_scan;
    uint64_t m_scanVersion = 1;

    std::atomic<bool> m_analyzing{false};
    std::mutex m_analysisMutex;
    AnalysisSnapshot m_analysis;
    uint64_t m_analysisVersion = 0;
};
