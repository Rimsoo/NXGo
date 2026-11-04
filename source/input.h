#pragma once

#include <switch.h>

// Entrées unifiées : manette (Joy-Con / Lite) et écran tactile.
//
// Le tactile suit un modèle "appui → glissement → relâchement" :
//  - `touching` est vrai tant que le doigt est posé, avec sa position courante ;
//  - au relâchement, `released` est vrai pour une image, avec la dernière position ;
//  - `tap` vaut released && le doigt a peu bougé (pas un glissement).
struct Input {
    u64 down = 0;   // boutons pressés à cette image
    u64 held = 0;   // boutons maintenus
    u64 repeat = 0; // `down` + répétition automatique pour les directions

    bool touching = false;
    bool touchStarted = false;
    bool released = false;
    bool tap = false;
    int x = 0, y = 0;            // position courante (ou dernière si relâché)
    int startX = 0, startY = 0;  // position au début de l'appui
    int dragX = 0, dragY = 0;    // déplacement depuis l'image précédente

    bool pressed(u64 buttons) const { return (down & buttons) != 0; }
    bool repeated(u64 buttons) const { return (repeat & buttons) != 0; }
    bool tapIn(int rx, int ry, int rw, int rh) const {
        return tap && x >= rx && x < rx + rw && y >= ry && y < ry + rh && startX >= rx && startX < rx + rw &&
               startY >= ry && startY < ry + rh;
    }
    bool touchIn(int rx, int ry, int rw, int rh) const {
        return touching && x >= rx && x < rx + rw && y >= ry && y < ry + rh;
    }
};

void input_init();
// Lit l'état de la manette et du tactile. Retourne l'état de cette image.
const Input& input_update();
