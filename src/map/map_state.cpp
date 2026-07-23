#include "map/map_state.h"

namespace MapState {

double centerLat = 42.96, centerLon = 1.37;
int zoom = 7, centerTX = 0, centerTY = 0;
int zoomMin = 6, zoomMax = 8;
int dragAccumX = 0, dragAccumY = 0;

double gpsLat = 0.0, gpsLon = 0.0;
bool mapFollowGps = true;

bool mapActive = false;
bool fullscreenMap = false;

} // namespace MapState
