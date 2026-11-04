#pragma once

// Réglages de l'appli, enregistrés dans sdmc:/switch/nxgo/config.txt (clé=valeur),
// et choix de la langue de l'interface.

#include <string>

enum class Lang { EN, FR };

struct Settings {
    Lang lang = Lang::EN;
    int analysisNet = 1;       // NetId : 0 b6, 1 b10, 2 b15
    int analysisVisits = 400;
    bool showCoords = true;
    bool showOwnership = false;  // territoire estimé pendant l'analyse

    // Derniers choix de "Nouvelle partie"
    int boardSize = 19;
    int color = 0;  // 0 Noir, 1 Blanc, 2 au hasard
    int aiLevel = 5;
    int handicap = 0;
    int rules = 0;  // 0 japonaises, 1 chinoises
};

extern Settings g_settings;

void settings_load();
void settings_save();

// Texte dans la langue choisie : L("Play", "Jouer").
inline const char* L(const char* en, const char* fr)
{
    return g_settings.lang == Lang::FR ? fr : en;
}

// printf vers une std::string.
std::string strf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static const char* const APP_DIR = "sdmc:/switch/nxgo";
static const char* const SGF_DIR = "sdmc:/switch/nxgo/sgf";
