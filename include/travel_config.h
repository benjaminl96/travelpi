#ifndef TRAVELPI_TRAVEL_CONFIG_H
#define TRAVELPI_TRAVEL_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "raylib.h"
#include "app_config.h"

typedef struct PhotoSpec {
    const char *path;
    Vector2 anchor;
    float scale;
    float rotation;
    float drift;
} PhotoSpec;

typedef struct GeoCoord {
    float latitude;
    float longitude;
} GeoCoord;

typedef struct TravelLocation {
    const char *name;
    const char *caption;
    const char *date_label;
    GeoCoord geo;
    Vector2 pixel_nudge;
    float close_zoom;
    float zoom_in_seconds;
    float hold_seconds;
    float fade_seconds;
    float zoom_out_seconds;
    int photo_count;
    const PhotoSpec *photos;
} TravelLocation;

extern const TravelLocation TRAVELPI_LOCATIONS[];
extern const size_t TRAVELPI_LOCATION_COUNT;

typedef struct RuntimeTravelConfig {
    TravelLocation *locations;
    size_t location_count;
} RuntimeTravelConfig;

bool LoadRuntimeTravelConfig(const char *path, RuntimeTravelConfig *config);
void UnloadRuntimeTravelConfig(RuntimeTravelConfig *config);

#endif
