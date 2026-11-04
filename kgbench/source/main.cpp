// kgbench: feasibility test for running KataGo (Eigen CPU backend) natively on Switch.
// Loads each model found in sdmc:/switch/nxgo/models/, searches an opening position
// for a fixed number of visits and reports load time, visits/s and the chosen move.
#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <malloc.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "core/config_parser.h"
#include "core/logger.h"
#include "core/rand.h"
#include "game/board.h"
#include "game/boardhistory.h"
#include "neuralnet/nneval.h"
#include "program/setup.h"
#include "search/search.h"

static const char* MODEL_DIRS[] = {"romfs:/models", "sdmc:/switch/nxgo/models"};

static double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static void show() {
  consoleUpdate(NULL);
  fflush(stdout);
}

static void runModel(const std::string& path, int threads, int visits) {
  printf("\n== %s\n   threads=%d visits=%d\n", path.c_str(), threads, visits);
  show();

  std::map<std::string, std::string> kvs = {
    {"logToStderr", "false"},
    {"logAllGTPCommunication", "false"},
    {"numSearchThreads", std::to_string(threads)},
    {"numNNServerThreadsPerModel", "1"},
    {"numEigenThreadsPerModel", std::to_string(threads)},
    {"nnMaxBatchSize", std::to_string(threads)},
    {"nnCacheSizePowerOfTwo", "16"},
    {"nnMutexPoolSizePowerOfTwo", "12"},
    {"maxVisits", std::to_string(visits)},
    {"rules", "japanese"},
  };
  ConfigParser cfg(kvs);
  Logger logger(&cfg, false, false, false, false);
  Rand seedRand;

  auto t0 = std::chrono::steady_clock::now();
  NNEvaluator* nnEval = Setup::initializeNNEvaluator(
    path, path, "", cfg, logger, seedRand, threads, 19, 19,
    Setup::MaxBatchSizeRequest::fromConcurrency(), false, false, Setup::SETUP_FOR_OTHER);
  printf("   load: %.1fs  (heap in use %zu MB)\n", secondsSince(t0), mallinfo().uordblks / (1024 * 1024));
  show();

  SearchParams params = Setup::loadSingleParams(cfg, Setup::SETUP_FOR_OTHER);
  params.maxVisits = visits;
  params.maxPlayouts = visits;
  params.maxTime = 60.0;

  Rules rules = Rules::parseRules("japanese");
  Board board(19, 19);
  // A small opening so the net has something non-trivial to evaluate.
  const char* moves[] = {"Q16", "D4", "Q3", "D16"};
  BoardHistory hist(board, P_BLACK, rules, 0, BoardHistoryModes());
  Player pla = P_BLACK;
  for (const char* m : moves) {
    Loc loc = Location::ofString(m, board);
    hist.makeBoardMoveAssumeLegal(board, loc, pla, NULL);
    pla = getOpp(pla);
  }

  Search* search = new Search(params, nnEval, &logger, "kgbench");
  search->setPosition(pla, board, hist);

  // Warm-up: first evaluations include one-time setup costs.
  t0 = std::chrono::steady_clock::now();
  search->runWholeSearch(pla);
  double secs = secondsSince(t0);
  int64_t v = search->getRootVisits();
  Loc best = search->getChosenMoveLoc();
  ReportedSearchValues values = search->getRootValuesRequireSuccess();
  printf("   search: %lld visits in %.2fs = %.1f visits/s\n", (long long)v, secs, v / secs);
  printf("   best: %s  winrate(%s)=%.1f%%  score=%.1f\n",
         Location::toString(best, board).c_str(), PlayerIO::playerToString(pla).c_str(),
         100.0 * (0.5 + 0.5 * values.winLossValue), values.lead);
  show();

  // Single raw NN evaluation latency (no search), what "instant hints" would cost.
  t0 = std::chrono::steady_clock::now();
  search->clearSearch();
  params.maxVisits = 1;
  search->setParams(params);
  search->setPosition(pla, board, hist);
  search->runWholeSearch(pla);
  printf("   1-visit eval: %.0f ms\n", 1000.0 * secondsSince(t0));
  show();

  delete search;
  delete nnEval;
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  consoleInit(NULL);
  socketInitializeDefault();
  nxlinkStdio();

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  printf("kgbench - KataGo on Switch feasibility test\n");
  printf("Applet mode: %s\n", appletGetAppletType() == AppletType_Application ? "no (full RAM)" : "YES (limited RAM)");
  u64 memTotal = 0, memUsed = 0;
  svcGetInfo(&memTotal, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&memUsed, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  printf("Process memory: %llu MB total, %llu MB used\n", (unsigned long long)(memTotal >> 20), (unsigned long long)(memUsed >> 20));
  show();

  Board::initHash();
  ScoreValue::initTables();

  romfsInit();
  std::vector<std::string> models;
  for (const char* dir : MODEL_DIRS) {
    if (DIR* d = opendir(dir)) {
      while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.find(".bin.gz") != std::string::npos || n.find(".txt.gz") != std::string::npos)
          models.push_back(std::string(dir) + "/" + n);
      }
      closedir(d);
    }
  }
  // Smallest files first: a model too big for the applet heap fails last.
  std::sort(models.begin(), models.end(), [](const std::string& a, const std::string& b) {
    struct stat sa, sb;
    stat(a.c_str(), &sa); stat(b.c_str(), &sb);
    return sa.st_size < sb.st_size;
  });
  if (models.empty())
    printf("No model found\n");

  for (const std::string& m : models) {
    for (int threads : {1, 3}) {
      try {
        runModel(m, threads, threads == 1 ? 20 : 60);
      } catch (const std::exception& e) {
        printf("   ERROR: %s\n", e.what());
      }
    }
  }

  printf("\nDone. Appuie sur + pour quitter.\n");
  show();
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  NeuralNet::globalCleanup();
  ScoreValue::freeTables();
  socketExit();
  consoleExit(NULL);
  return 0;
}
