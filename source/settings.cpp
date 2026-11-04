#include "settings.h"

#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <switch.h>

Settings g_settings;

static const char* CONFIG_FILE = "sdmc:/switch/nxgo/config.txt";

static Lang systemLanguage()
{
    Lang result = Lang::EN;
    u64 code = 0;
    SetLanguage lang;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang)) &&
            (lang == SetLanguage_FR || lang == SetLanguage_FRCA))
            result = Lang::FR;
        setExit();
    }
    return result;
}

void settings_load()
{
    g_settings.lang = systemLanguage();
    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        int n = atoi(val);
        if (!strcmp(key, "language"))
            g_settings.lang = !strcmp(val, "fr") ? Lang::FR : Lang::EN;
        else if (!strcmp(key, "analysis_net"))
            g_settings.analysisNet = n;
        else if (!strcmp(key, "analysis_visits"))
            g_settings.analysisVisits = n;
        else if (!strcmp(key, "coords"))
            g_settings.showCoords = n != 0;
        else if (!strcmp(key, "ownership"))
            g_settings.showOwnership = n != 0;
        else if (!strcmp(key, "board_size"))
            g_settings.boardSize = n;
        else if (!strcmp(key, "color"))
            g_settings.color = n;
        else if (!strcmp(key, "ai_level"))
            g_settings.aiLevel = n;
        else if (!strcmp(key, "handicap"))
            g_settings.handicap = n;
        else if (!strcmp(key, "rules"))
            g_settings.rules = n;
        else if (!strcmp(key, "ogs_size"))
            g_settings.ogsSize = n;
        else if (!strcmp(key, "ogs_speed"))
            g_settings.ogsSpeed = n;
        else if (!strcmp(key, "ogs_client_id"))
            g_settings.ogsClientPassword = val;
        else if (!strcmp(key, "ogs_user"))
            g_settings.ogsUser = val;
    }
    fclose(f);
}

void settings_save()
{
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    FILE* f = fopen(CONFIG_FILE, "w");
    if (!f)
        return;
    const Settings& s = g_settings;
    fprintf(f, "language=%s\n", s.lang == Lang::FR ? "fr" : "en");
    fprintf(f, "analysis_net=%d\nanalysis_visits=%d\n", s.analysisNet, s.analysisVisits);
    fprintf(f, "coords=%d\nownership=%d\n", s.showCoords, s.showOwnership);
    fprintf(f, "board_size=%d\ncolor=%d\nai_level=%d\nhandicap=%d\nrules=%d\n", s.boardSize, s.color, s.aiLevel,
            s.handicap, s.rules);
    fprintf(f, "ogs_size=%d\nogs_speed=%d\n", s.ogsSize, s.ogsSpeed);
    if (!s.ogsUser.empty())
        fprintf(f, "ogs_user=%s\n", s.ogsUser.c_str());
    if (!s.ogsClientPassword.empty())
        fprintf(f, "ogs_client_id=%s\n", s.ogsClientPassword.c_str());
    fclose(f);
}

std::string strf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}
