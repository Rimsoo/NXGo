#include "engine.h"

#include <sys/stat.h>

#include <cmath>
#include <map>

#include <switch.h>

#include "core/config_parser.h"
#include "core/logger.h"
#include "core/rand.h"
#include "neuralnet/nneval.h"
#include "program/setup.h"
#include "search/asyncbot.h"
#include "search/search.h"

#include "log.h"

// Un réseau chargé jusqu'à trois fois : un évaluateur par taille de plateau. Celui du
// 19x19 accepte toutes les tailles ; ceux du 9x9 et du 13x13 calculent sur la vraie
// taille du plateau, donc bien plus vite (jusqu'à 4x en 9x9).
static const int SLOT_COUNT = 3;
static const int SLOT_SIZES[SLOT_COUNT] = {9, 13, 19};

struct Engine::Net {
    std::string path;
    NNEvaluator* eval[SLOT_COUNT] = {};
    AsyncBot* bot[SLOT_COUNT] = {};  // bots de jeu (l'analyse a le sien)
    std::atomic<bool> ready[SLOT_COUNT] = {};
};

static const int SEARCH_THREADS = 3;

// Réglages par niveau. Les niveaux faibles jouent l'intuition brute du petit réseau
// avec de la température (des coups moins bons choisis plus souvent) ; les niveaux
// forts réfléchissent plus longtemps avec un réseau plus gros.
struct LevelSpec {
    NetId net;
    int64_t visits;
    double temperature;
};

static const LevelSpec LEVELS[AI_LEVEL_MAX + 1] = {
    {NetId::B6, 1, 1.0},     // (inutilisé)
    {NetId::B6, 1, 1.3},     // 1
    {NetId::B6, 1, 0.9},     // 2
    {NetId::B6, 1, 0.5},     // 3
    {NetId::B6, 8, 0.3},     // 4
    {NetId::B6, 24, 0.1},    // 5
    {NetId::B10, 12, 0.1},   // 6
    {NetId::B10, 40, 0.0},   // 7
    {NetId::B10, 100, 0.0},  // 8
    {NetId::B10, 250, 0.0},  // 9
    {NetId::B15, 150, 0.0},  // 10 (B10 x 500 sans le réseau B15)
};

static const char* NET_FILES[(int)NetId::Count] = {"b6", "b10", "b15"};

static bool fileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Cherche d'abord sur la carte SD (pour remplacer un réseau), puis dans l'appli.
static std::string findNet(NetId id)
{
    const char* dirs[] = {"sdmc:/switch/nxgo/models/", "romfs:/models/"};
    const char* exts[] = {".bin.gz", ".txt.gz"};
    for (const char* dir : dirs)
        for (const char* ext : exts) {
            std::string p = std::string(dir) + NET_FILES[(int)id] + ext;
            if (fileExists(p))
                return p;
        }
    return "";
}

Engine::Engine() = default;

Engine::~Engine()
{
    stopAll();
    m_abortLoad = true;
    if (m_loader.joinable())
        m_loader.join();
    delete m_analysisBot;
    for (auto& n : m_nets)
        if (n)
            for (int k = 0; k < SLOT_COUNT; k++) {
                delete n->bot[k];
                delete n->eval[k];
            }
}

const char* Engine::netName(NetId id)
{
    return NET_FILES[(int)id];
}

bool Engine::hasNet(NetId id) const
{
    return ready() && m_nets[(int)id] && m_nets[(int)id]->ready[SLOT_COUNT - 1];
}

int Engine::sizeSlot(int boardSize)
{
    return boardSize == 9 ? 0 : (boardSize == 13 ? 1 : 2);
}

NNEvaluator* Engine::evalFor(NetId id, int boardSize, AsyncBot** bot)
{
    if (!hasNet(id))
        return nullptr;
    Net& n = *m_nets[(int)id];
    int slot = sizeSlot(boardSize);
    if (!n.ready[slot])
        slot = SLOT_COUNT - 1;
    if (bot)
        *bot = n.bot[slot];
    return n.eval[slot];
}

void Engine::start()
{
    std::map<std::string, std::string> kvs = {
        {"logToStderr", "false"},
        {"numSearchThreads", std::to_string(SEARCH_THREADS)},
        {"numNNServerThreadsPerModel", "1"},
        {"numEigenThreadsPerModel", std::to_string(SEARCH_THREADS)},
        {"nnCacheSizePowerOfTwo", "15"},
        {"nnMutexPoolSizePowerOfTwo", "12"},
        {"maxVisits", "100"},
        {"rules", "japanese"},
    };
    m_cfg.reset(new ConfigParser(kvs));
    m_logger.reset(new Logger(m_cfg.get(), false, false, false, false));
    m_loading = true;
    m_loader = std::thread(&Engine::loadThread, this);
}

void Engine::loadSlot(Net& n, int id, int slot)
{
    Rand seedRand;
    int len = SLOT_SIZES[slot];
    bool exact = slot != SLOT_COUNT - 1;
    SearchParams params = Setup::loadSingleParams(*m_cfg, Setup::SETUP_FOR_OTHER);
    n.eval[slot] = Setup::initializeNNEvaluator(
        NET_FILES[id], n.path, "", *m_cfg, *m_logger, seedRand, SEARCH_THREADS, len, len,
        Setup::MaxBatchSizeRequest::fromConcurrency(), exact, false, Setup::SETUP_FOR_OTHER);
    n.bot[slot] = new AsyncBot(params, n.eval[slot], m_logger.get(), std::string("nxgo-") + NET_FILES[id]);
    n.ready[slot] = true;
    log_printf("Reseau %s %dx%d charge\n", NET_FILES[id], len, len);
}

void Engine::loadThread()
{
    try {
        // D'abord le 19x19 de chaque réseau (il sert pour toutes les tailles)...
        for (int i = 0; i < (int)NetId::Count; i++) {
            std::string path = findNet((NetId)i);
            if (path.empty())
                continue;
            auto n = std::make_unique<Net>();
            n->path = path;
            loadSlot(*n, i, SLOT_COUNT - 1);
            m_nets[i] = std::move(n);
        }
        if (!m_nets[(int)NetId::B6] && !m_nets[(int)NetId::B10] && !m_nets[(int)NetId::B15])
            m_error = "no network";
    } catch (const std::exception& e) {
        m_error = e.what();
        log_printf("Erreur de chargement KataGo : %s\n", e.what());
    }
    // Analyse : b10 par défaut (b15 est lent sur Switch)
    if (m_nets[(int)NetId::B10])
        m_analysisNet = NetId::B10;
    else if (m_nets[(int)NetId::B6])
        m_analysisNet = NetId::B6;
    else
        m_analysisNet = NetId::B15;
    m_loading = false;

    // ... puis les versions 9x9 et 13x13 en arrière-plan (b15 : 19x19 seulement, il est gros)
    for (int slot = 0; slot < SLOT_COUNT - 1; slot++)
        for (int i = 0; i < (int)NetId::B15; i++) {
            if (m_abortLoad || !m_error.empty() || !m_nets[i])
                continue;
            try {
                loadSlot(*m_nets[i], i, slot);
            } catch (const std::exception& e) {
                log_printf("Reseau %s %dx%d : %s\n", NET_FILES[i], SLOT_SIZES[slot], SLOT_SIZES[slot], e.what());
            }
        }
}

void Engine::setAnalysisNet(NetId id)
{
    if (!hasNet(id) || id == m_analysisNet)
        return;
    stopAnalysis();
    m_analysisNet = id;
}

AsyncBot* Engine::playBot(int level, int boardSize)
{
    LevelSpec spec = LEVELS[level];
    NetId usedNet = spec.net;
    if (!hasNet(usedNet) && usedNet == NetId::B15) {
        usedNet = NetId::B10;
        spec.visits = 500;
    }
    if (!hasNet(usedNet))
        usedNet = hasNet(NetId::B10) ? NetId::B10 : (hasNet(NetId::B6) ? NetId::B6 : NetId::B15);
    AsyncBot* bot = nullptr;
    if (!evalFor(usedNet, boardSize, &bot))
        return nullptr;

    SearchParams params = bot->getParams();
    params.maxVisits = spec.visits;
    params.maxPlayouts = spec.visits;
    params.maxTime = 20.0;
    params.chosenMoveTemperature = spec.temperature;
    params.chosenMoveTemperatureEarly = spec.temperature;
    params.numThreads = spec.visits < SEARCH_THREADS * 4 ? 1 : SEARCH_THREADS;
    params.conservativePass = true;
    bot->setParams(params);
    return bot;
}

void Engine::requestMove(const Board& board, const BoardHistory& hist, Player pla, int level, uint64_t positionId)
{
    stopAnalysis();
    cancelMove();
    if (level < AI_LEVEL_MIN)
        level = AI_LEVEL_MIN;
    if (level > AI_LEVEL_MAX)
        level = AI_LEVEL_MAX;
    AsyncBot* bot = playBot(level, board.x_size);
    if (!bot)
        return;

    m_moveBot = bot;
    m_moveReady = false;
    m_thinking = true;
    bot->setPosition(pla, board, hist);
    bot->genMoveAsync(pla, (int)(positionId & 0x7FFFFFFF), TimeControls(), 1.0,
                      [this, positionId, pla](Loc loc, int, Search* search) {
                          ReportedSearchValues values;
                          double whiteWin = 0.5;
                          if (search->getRootValues(values))
                              whiteWin = 0.5 * (1.0 + values.winLossValue);
                          m_moveLoc = loc;
                          m_moveWinrate = pla == P_WHITE ? whiteWin : 1.0 - whiteWin;
                          m_movePositionId = positionId;
                          m_moveReady = true;
                          m_thinking = false;
                      });
}

bool Engine::pollMove(Loc& loc, uint64_t& positionId, double& winrateForMover)
{
    if (!m_moveReady)
        return false;
    m_moveReady = false;
    loc = m_moveLoc;
    positionId = m_movePositionId;
    winrateForMover = m_moveWinrate;
    return true;
}

void Engine::cancelMove()
{
    if (m_moveBot) {
        m_moveBot->stopAndWait();
        m_moveBot = nullptr;
    }
    m_moveReady = false;
    m_thinking = false;
}

void Engine::startAnalysis(const Board& board, const BoardHistory& hist, Player pla, uint64_t positionId)
{
    if (m_thinking)
        return;
    stopAnalysis();
    AsyncBot* playBot = nullptr;
    NNEvaluator* eval = evalFor(m_analysisNet, board.x_size, &playBot);
    if (!eval)
        return;
    if (!m_analysisBot || m_analysisBotEval != eval) {
        delete m_analysisBot;
        m_analysisBot = new AsyncBot(playBot->getParams(), eval, m_logger.get(), "nxgo-analysis");
        m_analysisBot->setAlwaysIncludeOwnerMap(true);
        m_analysisBotEval = eval;
    }
    SearchParams params = playBot->getParams();
    params.maxVisits = m_analysisMaxVisits;
    params.maxPlayouts = m_analysisMaxVisits;
    params.maxTime = 1e20;
    params.numThreads = SEARCH_THREADS;
    params.chosenMoveTemperature = 0;
    params.chosenMoveTemperatureEarly = 0;
    params.wideRootNoise = 0.04;  // explore un peu plus de coups candidats, comme l'analyse de KataGo
    m_analysisBot->setParams(params);
    m_analysisBot->setPosition(pla, board, hist);

    {
        std::lock_guard<std::mutex> lock(m_analysisMutex);
        m_analysis = AnalysisSnapshot();
        m_analysis.positionId = positionId;
    }
    int boardSize = board.x_size;
    m_analyzing = true;
    m_analysisBot->analyzeAsync(pla, 1.0, 0.3, 0.15, [this, positionId, boardSize, pla](const Search* search) {
        publishAnalysis(search, positionId, boardSize, pla);
    });
}

void Engine::publishAnalysis(const void* searchPtr, uint64_t positionId, int boardSize, Player pla)
{
    const Search* search = static_cast<const Search*>(searchPtr);
    ReportedSearchValues values;
    if (!search->getRootValues(values))
        return;

    AnalysisSnapshot snap;
    snap.positionId = positionId;
    snap.visits = search->getRootVisits();
    snap.winrateBlack = 1.0 - 0.5 * (1.0 + values.winLossValue);
    snap.leadBlack = -values.lead;

    std::vector<AnalysisData> data;
    search->getAnalysisData(data, 0, false, 12, false);
    for (const AnalysisData& d : data) {
        AnalysisMove m;
        m.loc = d.move;
        m.visits = d.numVisits;
        double whiteWin = 0.5 * (1.0 + d.winLossValue);
        m.winrate = pla == P_WHITE ? whiteWin : 1.0 - whiteWin;
        m.lead = pla == P_WHITE ? d.lead : -d.lead;
        m.prior = d.policyPrior;
        m.order = d.order;
        m.pv = d.pv;
        snap.moves.push_back(std::move(m));
    }

    // Possession déjà ramenée à la taille du plateau (y * taille + x), du point de vue de Blanc
    std::vector<double> own = search->getAverageTreeOwnership(P_WHITE, NULL, 0);
    if ((int)own.size() == boardSize * boardSize)
        snap.ownership.assign(own.begin(), own.end());
    snap.valid = true;

    std::lock_guard<std::mutex> lock(m_analysisMutex);
    if (snap.positionId != m_analysis.positionId)
        return;  // résultat d'une ancienne position
    m_analysis = std::move(snap);
    m_analysisVersion++;
}

void Engine::stopAnalysis()
{
    if (m_analysisBot && m_analyzing)
        m_analysisBot->stopAndWait();
    m_analyzing = false;
}

bool Engine::getAnalysis(AnalysisSnapshot& out, uint64_t& version)
{
    std::lock_guard<std::mutex> lock(m_analysisMutex);
    if (m_analysisVersion == version)
        return false;
    out = m_analysis;
    version = m_analysisVersion;
    return true;
}

void Engine::stopAll()
{
    stopScan();
    stopAnalysis();
    cancelMove();
}

// ------------------------------------------------------------------ courbe de score

void Engine::startScan(const Board& initial, Player first, const Rules& rules, const std::vector<Move>& moves)
{
    stopScan();
    {
        std::lock_guard<std::mutex> lock(m_analysisMutex);
        m_scan.assign(moves.size() + 1, NAN);
        m_scanVersion++;
    }
    NetId net = NetId::B6;
    while (net != NetId::Count && !hasNet(net))
        net = (NetId)((int)net + 1);
    if (net == NetId::Count)
        return;
    NNEvaluator* eval = evalFor(net, initial.x_size, nullptr);
    if (!eval)
        return;
    m_scanAbort = false;
    m_scanning = true;
    m_scanThread = std::thread([this, eval, initial, first, rules, moves] {
        // Priorité plus basse que la recherche : l'analyse en cours reste fluide
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x30);
        Board board = initial;
        BoardHistory hist(board, first, rules, 0, BoardHistoryModes());
        Player pla = first;
        MiscNNInputParams params;
        for (size_t i = 0; i <= moves.size() && !m_scanAbort; i++) {
            if (i > 0) {
                const Move& mv = moves[i - 1];
                if (mv.loc != Board::PASS_LOC && board.colors[mv.loc] != C_EMPTY) {
                    pla = getOpp(mv.pla);
                    continue;
                }
                hist.makeBoardMoveAssumeLegal(board, mv.loc, mv.pla, NULL, true);
                pla = getOpp(mv.pla);
            }
            NNResultBuf buf;
            eval->evaluate(board, hist, pla, params, buf, false, false);
            float lead = buf.result ? -buf.result->whiteLead : NAN;
            std::lock_guard<std::mutex> lock(m_analysisMutex);
            if (i < m_scan.size()) {
                m_scan[i] = lead;
                m_scanVersion++;
            }
        }
        m_scanning = false;
    });
}

void Engine::stopScan()
{
    m_scanAbort = true;
    if (m_scanThread.joinable())
        m_scanThread.join();
    m_scanning = false;
}

bool Engine::getScan(std::vector<float>& leadBlack, uint64_t& version)
{
    std::lock_guard<std::mutex> lock(m_analysisMutex);
    if (m_scanVersion == version)
        return false;
    leadBlack = m_scan;
    version = m_scanVersion;
    return true;
}
