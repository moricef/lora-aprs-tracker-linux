#pragma once
#include "lvgl/lvgl.h"
#include <cstdint>
#include <vector>
#include <string>

namespace MapVector {
    // Un label extrait d'une tuile, en coords tuile-locales (0..tileSz).
    // Le placement/collision se fait au niveau écran (map_raster), pas dans la tuile.
    struct Label {
        int px, py;                 // position dans la tuile (px, à tileSz=256)
        int priority;               // plus petit = plus important (placé en premier)
        std::string text;
        uint8_t r, g, b;
        const lv_font_t *font;
    };

    // Ouvre le fichier .pmtiles (mmap) et garde une référence globale.
    // Retourne true si OK. À appeler une fois au démarrage.
    bool open(const char *pmtilesPath);

    // Rend la tile z/x/y sur un lv_canvas existant (taille canvas = canvasSize px).
    // Le canvas doit être pré-créé avec un buffer de taille canvasSize*canvasSize*4 (ARGB32)
    // ou via lv_canvas_set_buffer.
    // Retourne true si la tile a été trouvée et rendue, false si tile vide/absente.
    bool renderTile(lv_obj_t *canvas, int z, int x, int y, int canvasSize);

    // Remplit buf (sz*sz*4, ARGB8888 octets B,G,R,A) avec le rendu de la tuile.
    // Outil de debug : pixels identiques à ce que le canvas afficherait.
    bool renderTileRaw(uint8_t *buf, int sz, int z, int x, int y);

    // Extrait les labels d'une tuile (lieux, lac, cours d'eau, refs de route) en coords
    // tuile-locales, SANS les dessiner. Le placement global est fait par l'appelant.
    void getTileLabels(int z, int x, int y, std::vector<Label> &out);

    bool isOpen();
    int  minZoom();
    int  maxZoom();
    void startWorker();
    void requestTile(int z, int x, int y);
    void stopWorker();
    void close();
}
