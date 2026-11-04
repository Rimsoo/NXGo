// NXGo : le go sur Switch, avec KataGo pour adversaire et pour l'analyse.

#include <switch.h>

#include "engine.h"
#include "gfx.h"
#include "input.h"
#include "log.h"
#include "settings.h"
#include "ui.h"

#include "neuralnet/nneval.h"
#include "search/search.h"

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    log_init();
    // L'interface passe avant les threads de KataGo (priorité 0x2C par défaut) :
    // elle reste fluide même quand l'analyse occupe les trois cœurs.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x28);
    romfsInit();
    settings_load();
    input_init();

    Board::initHash();
    ScoreValue::initTables();

    if (!gfx_init()) {
        log_printf("Impossible d'initialiser l'affichage\n");
        romfsExit();
        log_exit();
        return 1;
    }

    {
        Engine engine;
        engine.start();
        App app(engine);
        while (appletMainLoop() && app.frame()) {
        }
        engine.stopAll();
    }  // l'Engine libère KataGo avant le nettoyage global

    NeuralNet::globalCleanup();
    ScoreValue::freeTables();
    gfx_exit();
    romfsExit();
    log_exit();
    return 0;
}
