#pragma once
#include "lvgl/lvgl.h"
#include <cstdint>

namespace MapVector {
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

    bool isOpen();
    int  minZoom();
    int  maxZoom();
    void startWorker();
    void requestTile(int z, int x, int y);
    void stopWorker();
    void close();
}
