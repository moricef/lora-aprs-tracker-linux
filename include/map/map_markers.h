#pragma once

#include "lvgl.h"

// Station markers (own + received) and the station-info popup. Markers are
// drawn directly into the shared map canvas at sprite coordinates.

namespace MapMarkers {

// Draw own station + every valid MapStations entry into `canvas`, recording
// each marker's sprite position for hit-testing. Calls cleanOldMapStations.
void drawInto(lv_obj_t *canvas);

// Reset the marker list (markers are repainted by the next drawInto).
void deleteMarkers();

// Returns true if `point` (container coords) lies inside any marker's bbox;
// stores the stationIdx (-1 = own, >=0 mapStations).
bool hitTest(lv_point_t point, int *stationIdx);

// Station info modal (lv_msgbox). closeStationPopup is a no-op if none.
void showStationPopup(int stationIdx);
void closeStationPopup();

} // namespace MapMarkers
