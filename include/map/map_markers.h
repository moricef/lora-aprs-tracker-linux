#pragma once

#include "lvgl.h"

// Station markers (own + received) and the station-info popup. Marker
// widgets are children of MapMarkers::parent set by map_view at init.

namespace MapMarkers {

// Parent container (mapCont). Must be set before any create/update call.
extern lv_obj_t *parent;

// (Re)create all marker widgets : own station + every valid MapStations
// entry. Calls STATION_Utils::cleanOldMapStations first.
void createMarkers();

// Pan/zoom — move marker widgets in place without recreating them.
void updateMarkerPositions();

// Move only the own-station marker (gpsLat/gpsLon). Returns false if no
// own-marker exists yet — the caller can then call createMarkers().
bool updateOwnMarker();

// Free all marker widgets and reset the marker count.
void deleteMarkers();

// Returns true if `point` lies inside any marker's bbox; stores the
// stationIdx (-1 = own, >=0 mapStations).
bool hitTest(lv_point_t point, int *stationIdx);

// Station info modal (lv_msgbox). closeStationPopup is a no-op if none.
void showStationPopup(int stationIdx);
void closeStationPopup();

} // namespace MapMarkers
