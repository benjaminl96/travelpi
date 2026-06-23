#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "raylib.h"
#include "app_config.h"
#include "travel_config.h"

typedef enum TripPhase {
    TRIP_ZOOM_IN = 0,
    TRIP_PHOTOS_IN,
    TRIP_HOLD,
    TRIP_PHOTOS_OUT,
    TRIP_ZOOM_OUT,
} TripPhase;

typedef struct AppOptions {
    int width;
    int height;
    bool fullscreen;
    bool show_fps;
    bool show_profile;
    const char *start_name;
    float screenshot_after;
    float exit_after;
    const char *screenshot_path;
} AppOptions;

typedef struct TextureSlot {
    Texture2D texture;
    bool loaded;
} TextureSlot;

typedef struct PhotoBank {
    TextureSlot photos[TRAVELPI_MAX_PHOTOS_PER_LOCATION];
    size_t location_index;
    int page_index;
    int target_count;
    int cursor;
    bool valid;
} PhotoBank;

typedef struct MapTileSlot {
    TextureSlot texture;
    int tile_x;
    int tile_y;
    unsigned int last_used;
    bool valid;
} MapTileSlot;

typedef struct MapTileCache {
    MapTileSlot slots[TRAVELPI_MAP_TILE_CACHE_CAPACITY];
    int source_width;
    int source_height;
    int tile_size;
    int columns;
    int rows;
    float scale_x;
    float scale_y;
    unsigned int frame_id;
    int loads_this_frame;
    bool enabled;
} MapTileCache;

typedef struct MapTileRange {
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    bool valid;
} MapTileRange;

typedef struct SmoothCamera {
    Vector2 target;
    float zoom;
} SmoothCamera;

typedef struct FrameProfiler {
    float frame_ms;
    float update_ms;
    float draw_ms;
    float avg_ms;
    float worst_ms;
    float accumulator_ms;
    int sample_count;
} FrameProfiler;

typedef struct TravelRuntime {
    TextureSlot map;
    TextureSlot paper;
    Font banner_font;
    bool banner_font_loaded;
    MapTileCache tiles;
    PhotoBank current_bank;
    size_t location_index;
    int page_index;
    TripPhase phase;
    float phase_time;
    float photo_alpha;
    SmoothCamera camera;
    Vector2 macro_target;
    float macro_zoom;
    float clock;
    FrameProfiler profiler;
} TravelRuntime;

static const TravelLocation *g_locations = NULL;
static size_t g_location_count = 0;

static time_t FileModifiedTime(const char *path)
{
    struct stat status;

    if (stat(path, &status) != 0) {
        return 0;
    }

    return status.st_mtime;
}

static float Clamp01(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static float SmoothStep01(float value)
{
    const float t = Clamp01(value);
    return t*t*(3.0f - 2.0f*t);
}

static float LerpFloat(float from, float to, float amount)
{
    return from + (to - from)*amount;
}

static Vector2 LerpVector2(Vector2 from, Vector2 to, float amount)
{
    return (Vector2) {
        LerpFloat(from.x, to.x, amount),
        LerpFloat(from.y, to.y, amount),
    };
}

static float MinFloat(float a, float b)
{
    return (a < b) ? a : b;
}

static float MaxFloat(float a, float b)
{
    return (a > b) ? a : b;
}

static float BottomBandHeight(int screen_height)
{
    return MaxFloat((float)screen_height*0.072f, 78.0f);
}

static int ClampInt(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static Rectangle InsetRectangle(Rectangle rect, float amount)
{
    return (Rectangle) {
        rect.x + amount,
        rect.y + amount,
        MaxFloat(rect.width - amount*2.0f, 1.0f),
        MaxFloat(rect.height - amount*2.0f, 1.0f),
    };
}

static unsigned char ColorByteFromInt(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (unsigned char)value;
}

static unsigned int HashU32(unsigned int value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

static Texture2D UploadCheckedTexture(int width, int height, Color a, Color b);
static void UnloadTextureSlot(TextureSlot *slot);

static bool LoadBannerFont(Font *font)
{
    if (!FileExists(TRAVELPI_BANNER_FONT_PATH)) {
        return false;
    }

    *font = LoadFontEx(TRAVELPI_BANNER_FONT_PATH, TRAVELPI_BANNER_FONT_BASE_SIZE, NULL, 0);

    if (font->texture.id == 0) {
        return false;
    }

    SetTextureFilter(font->texture, TEXTURE_FILTER_BILINEAR);
    return true;
}

static Vector2 MeasureBannerText(const TravelRuntime *runtime, const char *text, float size)
{
    if (runtime->banner_font_loaded) {
        return MeasureTextEx(runtime->banner_font, text, size, 0.0f);
    }

    return (Vector2) { (float)MeasureText(text, (int)size), size };
}

static void DrawBannerText(const TravelRuntime *runtime, const char *text, Vector2 position, float size, Color color)
{
    if (runtime->banner_font_loaded) {
        DrawTextEx(runtime->banner_font, text, position, size, 0.0f, color);
    } else {
        DrawText(text, (int)position.x, (int)position.y, (int)size, color);
    }
}

static void DrawBannerTextBold(const TravelRuntime *runtime, const char *text, Vector2 position, float size, Color color)
{
    const Color shadow = Fade((Color) { 24, 20, 14, 255 }, color.a*0.62f);

    DrawBannerText(runtime, text, (Vector2) { position.x + 1.0f, position.y + 1.0f }, size, shadow);
    DrawBannerText(runtime, text, (Vector2) { position.x + 0.5f, position.y }, size, shadow);
    DrawBannerText(runtime, text, (Vector2) { position.x, position.y + 0.5f }, size, shadow);
    DrawBannerText(runtime, text, position, size, color);
}

static void DrawBannerTextBoldRightAligned(const TravelRuntime *runtime, const char *text, float right_edge, float y, float size, Color color)
{
    const float width = MeasureBannerText(runtime, text, size).x;
    DrawBannerTextBold(runtime, text, (Vector2) { right_edge - width, y }, size, color);
}

static int PhotoGridColumnCount(int count)
{
    if (count <= 1) return 1;
    if (count <= 3) return count;
    if (count == 4) return 2;
    return 3;
}

static void BuildPhotoGrid(Rectangle *cells, int count, Rectangle area, float gap)
{
    const int columns = PhotoGridColumnCount(count);
    const int rows = (count + columns - 1)/columns;
    const float cell_width = (area.width - gap*(float)(columns - 1))/(float)columns;
    const float cell_height = (area.height - gap*(float)(rows - 1))/(float)rows;
    int cursor = 0;

    for (int row = 0; row < rows; ++row) {
        const int remaining = count - cursor;
        const int row_count = (remaining < columns) ? remaining : columns;
        const float row_width = cell_width*(float)row_count + gap*(float)(row_count - 1);
        const float row_left = area.x + (area.width - row_width)*0.5f;

        for (int column = 0; column < row_count; ++column) {
            cells[cursor++] = (Rectangle) {
                row_left + (cell_width + gap)*(float)column,
                area.y + (cell_height + gap)*(float)row,
                cell_width,
                cell_height,
            };
        }
    }
}

static TextureSlot CreatePaperTexture(void)
{
    enum { PAPER_SIZE = 256 };
    TextureSlot slot = { 0 };
    Color *pixels = (Color *)malloc(PAPER_SIZE*PAPER_SIZE*sizeof(Color));

    if (pixels == NULL) {
        slot.texture = UploadCheckedTexture(
            PAPER_SIZE,
            PAPER_SIZE,
            (Color) { 194, 171, 121, 255 },
            (Color) { 178, 151, 100, 255 });
        slot.loaded = (slot.texture.id != 0);
        return slot;
    }

    for (int y = 0; y < PAPER_SIZE; ++y) {
        const int fiber = (int)(HashU32((unsigned int)y*241u + 19u) & 15u) - 7;

        for (int x = 0; x < PAPER_SIZE; ++x) {
            const unsigned int hash = HashU32((unsigned int)x*1973u + (unsigned int)y*9277u + 131u);
            const int grain = (int)(hash & 31u) - 15;
            int fleck = 0;

            if ((hash & 1023u) < 8u) {
                fleck = ((hash & 2048u) != 0u) ? 28 : -32;
            }

            const int value = grain + fiber + fleck;
            pixels[y*PAPER_SIZE + x] = (Color) {
                ColorByteFromInt(194 + value),
                ColorByteFromInt(171 + value),
                ColorByteFromInt(121 + value/2),
                255,
            };
        }
    }

    Image image = {
        .data = pixels,
        .width = PAPER_SIZE,
        .height = PAPER_SIZE,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };

    slot.texture = LoadTextureFromImage(image);
    free(pixels);

    if (slot.texture.id != 0) {
        SetTextureFilter(slot.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(slot.texture, TEXTURE_WRAP_REPEAT);
    }

    slot.loaded = (slot.texture.id != 0);
    return slot;
}

static const char *AssetPath(const char *relative_path)
{
    static char buffers[8][512];
    static int slot = 0;
    const char *root = getenv("TRAVELPI_ASSET_ROOT");

    if ((root == NULL) || (root[0] == '\0')) {
        return relative_path;
    }

    char *buffer = buffers[slot];
    slot = (slot + 1) % 8;
    snprintf(buffer, 512, "%s/%s", root, relative_path);
    return buffer;
}

static Texture2D UploadCheckedTexture(int width, int height, Color a, Color b)
{
    Image image = GenImageChecked(width, height, 64, 64, a, b);
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
    return texture;
}

static Texture2D LoadMapImageTexture(const char *path)
{
    Texture2D texture = { 0 };
    Image image = LoadImage(path);

    if (image.data == NULL) {
        return texture;
    }

    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R5G6B5);
    texture = LoadTextureFromImage(image);
    UnloadImage(image);
    return texture;
}

static TextureSlot LoadMapTexture(void)
{
    TextureSlot slot = { 0 };
    const char *path = AssetPath(TRAVELPI_MAP_PATH);
    const char *fallback_path = AssetPath(TRAVELPI_MAP_FALLBACK_PATH);

    if (FileExists(path)) {
        slot.texture = LoadMapImageTexture(path);
        SetTextureFilter(slot.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(slot.texture, TEXTURE_WRAP_CLAMP);
    } else if (FileExists(fallback_path)) {
        slot.texture = LoadMapImageTexture(fallback_path);
        SetTextureFilter(slot.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(slot.texture, TEXTURE_WRAP_CLAMP);
    } else {
        slot.texture = UploadCheckedTexture(
            TRAVELPI_PLACEHOLDER_MAP_WIDTH,
            TRAVELPI_PLACEHOLDER_MAP_HEIGHT,
            (Color) { 38, 47, 54, 255 },
            (Color) { 50, 61, 68, 255 });
    }

    slot.loaded = (slot.texture.id != 0);
    return slot;
}

static const char *MapTilePath(const char *format, int tile_x, int tile_y)
{
    static char buffers[4][256];
    static int slot = 0;
    char *buffer = buffers[slot];
    slot = (slot + 1) % 4;
    snprintf(buffer, 256, format, tile_x, tile_y);
    return AssetPath(buffer);
}

static const char *SiblingQoiPath(const char *relative_path)
{
    static char buffers[4][512];
    static int slot = 0;
    char *buffer = buffers[slot];
    const char *dot = strrchr(relative_path, '.');
    size_t prefix_length = (dot == NULL) ? strlen(relative_path) : (size_t)(dot - relative_path);

    slot = (slot + 1) % 4;
    if (prefix_length > 500) {
        prefix_length = 500;
    }

    snprintf(buffer, 512, "%.*s.qoi", (int)prefix_length, relative_path);
    return AssetPath(buffer);
}

static TextureSlot LoadMapTileTexture(int tile_x, int tile_y)
{
    TextureSlot slot = { 0 };
    const char *path = MapTilePath(TRAVELPI_MAP_TILE_PATH_FORMAT, tile_x, tile_y);

    if (!FileExists(path)) {
        path = MapTilePath(TRAVELPI_MAP_TILE_FALLBACK_PATH_FORMAT, tile_x, tile_y);
    }

    if (FileExists(path)) {
        slot.texture = LoadMapImageTexture(path);
        SetTextureFilter(slot.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(slot.texture, TEXTURE_WRAP_CLAMP);
    }

    slot.loaded = (slot.texture.id != 0);
    return slot;
}

static void ResetMapTileCache(MapTileCache *cache)
{
    memset(cache, 0, sizeof(*cache));

    for (int i = 0; i < TRAVELPI_MAP_TILE_CACHE_CAPACITY; ++i) {
        cache->slots[i].tile_x = -1;
        cache->slots[i].tile_y = -1;
    }
}

static void UnloadMapTileCache(MapTileCache *cache)
{
    for (int i = 0; i < TRAVELPI_MAP_TILE_CACHE_CAPACITY; ++i) {
        UnloadTextureSlot(&cache->slots[i].texture);
    }

    ResetMapTileCache(cache);
}

static bool LoadMapTileMetadata(MapTileCache *cache, const Texture2D base_map)
{
    FILE *file = fopen(AssetPath(TRAVELPI_MAP_TILE_METADATA_PATH), "r");
    char line[128];
    int source_width = 0;
    int source_height = 0;
    int tile_size = 0;
    int columns = 0;
    int rows = 0;

    ResetMapTileCache(cache);

    if (file == NULL) {
        return false;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        sscanf(line, "width=%d", &source_width);
        sscanf(line, "height=%d", &source_height);
        sscanf(line, "tile_size=%d", &tile_size);
        sscanf(line, "columns=%d", &columns);
        sscanf(line, "rows=%d", &rows);
    }

    fclose(file);

    if (source_width <= 0 || source_height <= 0 || tile_size <= 0 ||
        columns <= 0 || rows <= 0 || base_map.width <= 0 || base_map.height <= 0) {
        ResetMapTileCache(cache);
        return false;
    }

    cache->source_width = source_width;
    cache->source_height = source_height;
    cache->tile_size = tile_size;
    cache->columns = columns;
    cache->rows = rows;
    cache->scale_x = (float)source_width/(float)base_map.width;
    cache->scale_y = (float)source_height/(float)base_map.height;
    cache->enabled = true;
    return true;
}

static MapTileSlot *FindMapTileSlot(MapTileCache *cache, int tile_x, int tile_y)
{
    for (int i = 0; i < TRAVELPI_MAP_TILE_CACHE_CAPACITY; ++i) {
        MapTileSlot *slot = &cache->slots[i];

        if (slot->valid && slot->tile_x == tile_x && slot->tile_y == tile_y) {
            return slot;
        }
    }

    return NULL;
}

static MapTileSlot *ChooseMapTileSlot(MapTileCache *cache)
{
    MapTileSlot *oldest = &cache->slots[0];

    for (int i = 0; i < TRAVELPI_MAP_TILE_CACHE_CAPACITY; ++i) {
        MapTileSlot *slot = &cache->slots[i];

        if (!slot->valid) {
            return slot;
        }

        if (slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }

    UnloadTextureSlot(&oldest->texture);
    oldest->valid = false;
    oldest->tile_x = -1;
    oldest->tile_y = -1;
    return oldest;
}

static MapTileSlot *GetMapTileSlot(MapTileCache *cache, int tile_x, int tile_y)
{
    MapTileSlot *slot = FindMapTileSlot(cache, tile_x, tile_y);

    if (slot != NULL) {
        slot->last_used = cache->frame_id;
        return slot;
    }

    if (cache->loads_this_frame >= TRAVELPI_MAP_TILE_LOADS_PER_FRAME) {
        return NULL;
    }

    slot = ChooseMapTileSlot(cache);
    slot->texture = LoadMapTileTexture(tile_x, tile_y);
    cache->loads_this_frame++;

    if (!slot->texture.loaded) {
        return NULL;
    }

    slot->tile_x = tile_x;
    slot->tile_y = tile_y;
    slot->last_used = cache->frame_id;
    slot->valid = true;
    return slot;
}

static TextureSlot LoadPhotoTexture(const char *relative_path, int fallback_index)
{
    TextureSlot slot = { 0 };
    const char *path = SiblingQoiPath(relative_path);

    if (!FileExists(path)) {
        path = AssetPath(relative_path);
    }

    if (FileExists(path)) {
        Image image = LoadImage(path);
        slot.texture = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureFilter(slot.texture, TEXTURE_FILTER_BILINEAR);
    } else {
        const Color palette_a[] = {
            { 195, 217, 217, 255 },
            { 218, 197, 177, 255 },
            { 213, 210, 186, 255 },
            { 188, 202, 226, 255 },
        };
        const Color palette_b[] = {
            { 68, 91, 103, 255 },
            { 116, 79, 67, 255 },
            { 91, 104, 76, 255 },
            { 69, 84, 125, 255 },
        };
        const int i = fallback_index % 4;
        slot.texture = UploadCheckedTexture(640, 426, palette_a[i], palette_b[i]);
    }

    slot.loaded = (slot.texture.id != 0);
    return slot;
}

static void UnloadTextureSlot(TextureSlot *slot)
{
    if (slot->loaded && (slot->texture.id != 0)) {
        UnloadTexture(slot->texture);
    }
    memset(slot, 0, sizeof(*slot));
}

static void ResetPhotoBank(PhotoBank *bank)
{
    memset(bank, 0, sizeof(*bank));
    bank->location_index = (size_t)-1;
    bank->page_index = -1;
}

static void UnloadPhotoBank(PhotoBank *bank)
{
    for (int i = 0; i < TRAVELPI_MAX_PHOTOS_PER_LOCATION; ++i) {
        UnloadTextureSlot(&bank->photos[i]);
    }
    ResetPhotoBank(bank);
}

static const TravelLocation *CurrentLocation(const TravelRuntime *runtime)
{
    return &g_locations[runtime->location_index % g_location_count];
}

static size_t FindLocationIndex(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) {
        return 0;
    }

    for (size_t i = 0; i < g_location_count; ++i) {
        if (strcmp(g_locations[i].name, name) == 0) {
            return i;
        }
    }

    fprintf(stderr, "travelpi: start location not found: %s\n", name);
    return 0;
}

static float FitMapZoom(const Texture2D map, int screen_width, int screen_height)
{
    const float fit_x = (float)screen_width/(float)map.width;
    const float fit_y = (float)screen_height/(float)map.height;
    const float fit = (fit_x < fit_y) ? fit_x : fit_y;
    return fit*TRAVELPI_MACRO_MAP_MARGIN;
}

static Vector2 ProjectGeoToMap(const TravelLocation *location, const Texture2D map)
{
    const float x = ((location->geo.longitude + 180.0f)/360.0f)*(float)map.width;
    const float y = ((90.0f - location->geo.latitude)/180.0f)*(float)map.height;

    return (Vector2) {
        x + location->pixel_nudge.x,
        y + location->pixel_nudge.y,
    };
}

static int PageCountForLocation(const TravelLocation *location)
{
    if (location->photo_count <= 0) {
        return 1;
    }

    return (location->photo_count + TRAVELPI_MAX_PHOTOS_PER_LOCATION - 1)/TRAVELPI_MAX_PHOTOS_PER_LOCATION;
}

static int PhotoStartForPage(int page_index)
{
    return page_index*TRAVELPI_MAX_PHOTOS_PER_LOCATION;
}

static int PhotoCountForPage(const TravelLocation *location, int page_index)
{
    const int start = PhotoStartForPage(page_index);
    int remaining = location->photo_count - start;

    if (remaining < 0) {
        remaining = 0;
    }

    return (remaining < TRAVELPI_MAX_PHOTOS_PER_LOCATION) ? remaining : TRAVELPI_MAX_PHOTOS_PER_LOCATION;
}

static void LoadPhotoBankBlocking(PhotoBank *bank, size_t location_index, int page_index)
{
    const TravelLocation *location = &g_locations[location_index % g_location_count];
    UnloadPhotoBank(bank);
    bank->location_index = location_index % g_location_count;
    bank->page_index = page_index;
    bank->target_count = PhotoCountForPage(location, page_index);

    for (int i = 0; i < bank->target_count; ++i) {
        const int photo_index = PhotoStartForPage(page_index) + i;
        bank->photos[i] = LoadPhotoTexture(location->photos[photo_index].path, i);
        bank->cursor = i + 1;
    }

    bank->valid = true;
}

static void BeginPage(TravelRuntime *runtime, size_t location_index, int page_index)
{
    location_index %= g_location_count;
    runtime->location_index = location_index;
    runtime->page_index = page_index;
    runtime->phase = TRIP_ZOOM_IN;
    runtime->phase_time = 0.0f;
    runtime->photo_alpha = 0.0f;

    LoadPhotoBankBlocking(&runtime->current_bank, location_index, page_index);
}

static void BeginLocation(TravelRuntime *runtime, size_t location_index)
{
    BeginPage(runtime, location_index, 0);
}

static bool ReloadRuntimeTrips(TravelRuntime *runtime, RuntimeTravelConfig *active_config, const char *start_name)
{
    RuntimeTravelConfig next_config = { 0 };

    if (!LoadRuntimeTravelConfig(TRAVELPI_TRIPS_CONFIG_PATH, &next_config) || next_config.location_count == 0) {
        UnloadRuntimeTravelConfig(&next_config);
        fprintf(stderr, "travelpi: ignored invalid trip config reload\n");
        return false;
    }

    UnloadPhotoBank(&runtime->current_bank);
    UnloadRuntimeTravelConfig(active_config);
    *active_config = next_config;
    g_locations = active_config->locations;
    g_location_count = active_config->location_count;
    BeginLocation(runtime, FindLocationIndex(start_name));
    fprintf(stderr, "travelpi: reloaded trips from %s\n", TRAVELPI_TRIPS_CONFIG_PATH);
    return true;
}

static void ResetPhase(TravelRuntime *runtime, TripPhase next_phase)
{
    runtime->phase = next_phase;
    runtime->phase_time = 0.0f;
}

static void UpdatePhase(TravelRuntime *runtime, float dt)
{
    const TravelLocation *location = CurrentLocation(runtime);
    runtime->phase_time += dt;
    runtime->clock += dt;

    switch (runtime->phase) {
        case TRIP_ZOOM_IN:
            runtime->photo_alpha = 0.0f;
            if (runtime->phase_time >= location->zoom_in_seconds) {
                ResetPhase(runtime, TRIP_PHOTOS_IN);
            }
            break;
        case TRIP_PHOTOS_IN:
            runtime->photo_alpha = SmoothStep01(runtime->phase_time/location->fade_seconds);
            if (runtime->phase_time >= location->fade_seconds) {
                ResetPhase(runtime, TRIP_HOLD);
                runtime->photo_alpha = 1.0f;
            }
            break;
        case TRIP_HOLD:
            runtime->photo_alpha = 1.0f;
            if (runtime->phase_time >= location->hold_seconds) {
                ResetPhase(runtime, TRIP_PHOTOS_OUT);
            }
            break;
        case TRIP_PHOTOS_OUT:
            runtime->photo_alpha = 1.0f - SmoothStep01(runtime->phase_time/location->fade_seconds);
            if (runtime->phase_time >= location->fade_seconds) {
                runtime->photo_alpha = 0.0f;
                if ((runtime->page_index + 1) < PageCountForLocation(location)) {
                    BeginPage(runtime, runtime->location_index, runtime->page_index + 1);
                    ResetPhase(runtime, TRIP_PHOTOS_IN);
                } else {
                    ResetPhase(runtime, TRIP_ZOOM_OUT);
                }
            }
            break;
        case TRIP_ZOOM_OUT:
            runtime->photo_alpha = 0.0f;
            if (runtime->current_bank.valid) {
                UnloadPhotoBank(&runtime->current_bank);
            }
            if (runtime->phase_time >= location->zoom_out_seconds) {
                BeginLocation(runtime, runtime->location_index + 1);
                ResetPhase(runtime, TRIP_PHOTOS_IN);
            }
            break;
    }
}

static void UpdateSmoothCamera(TravelRuntime *runtime, float dt)
{
    const TravelLocation *location = CurrentLocation(runtime);
    const bool changing_locations = (runtime->phase == TRIP_ZOOM_OUT);
    const TravelLocation *target_location = location;

    if (changing_locations) {
        target_location = &g_locations[(runtime->location_index + 1) % g_location_count];
    }

    const Vector2 desired_target = ProjectGeoToMap(target_location, runtime->map.texture);
    const float desired_zoom = target_location->close_zoom;
    const float camera_blend = 1.0f - expf(-TRAVELPI_CAMERA_RESPONSE*dt);
    const float zoom_blend = 1.0f - expf(-TRAVELPI_ZOOM_RESPONSE*dt);

    runtime->camera.target = LerpVector2(runtime->camera.target, desired_target, camera_blend);
    runtime->camera.zoom = LerpFloat(runtime->camera.zoom, desired_zoom, zoom_blend);
}

static void DrawPaperTexture(const TravelRuntime *runtime, Rectangle area, float alpha)
{
    if (!runtime->paper.loaded || (runtime->paper.texture.id == 0)) {
        DrawRectangleRec(area, Fade((Color) { 194, 171, 121, 255 }, alpha));
        return;
    }

    DrawTexturePro(
        runtime->paper.texture,
        (Rectangle) { area.x, area.y, area.width, area.height },
        area,
        (Vector2) { 0.0f, 0.0f },
        0.0f,
        Fade(WHITE, alpha));
}

static MapTileRange MapTileRangeForView(
    const TravelRuntime *runtime,
    Vector2 target,
    float zoom,
    int screen_width,
    int screen_height)
{
    const MapTileCache *cache = &runtime->tiles;
    MapTileRange range = { 0 };

    if (!cache->enabled || cache->tile_size <= 0 || zoom <= 0.001f) {
        return range;
    }

    const float half_width = ((float)screen_width*0.5f)/zoom;
    const float half_height = ((float)screen_height*0.5f)/zoom;
    const float source_left = MaxFloat((target.x - half_width)*cache->scale_x, 0.0f);
    const float source_top = MaxFloat((target.y - half_height)*cache->scale_y, 0.0f);
    const float source_right = MinFloat((target.x + half_width)*cache->scale_x, (float)cache->source_width);
    const float source_bottom = MinFloat((target.y + half_height)*cache->scale_y, (float)cache->source_height);

    range.min_x = ClampInt((int)floorf(source_left/(float)cache->tile_size) - TRAVELPI_MAP_TILE_PREFETCH_MARGIN, 0, cache->columns - 1);
    range.min_y = ClampInt((int)floorf(source_top/(float)cache->tile_size) - TRAVELPI_MAP_TILE_PREFETCH_MARGIN, 0, cache->rows - 1);
    range.max_x = ClampInt((int)floorf(source_right/(float)cache->tile_size) + TRAVELPI_MAP_TILE_PREFETCH_MARGIN, 0, cache->columns - 1);
    range.max_y = ClampInt((int)floorf(source_bottom/(float)cache->tile_size) + TRAVELPI_MAP_TILE_PREFETCH_MARGIN, 0, cache->rows - 1);
    range.valid = true;
    return range;
}

static MapTileRange VisibleMapTileRange(const TravelRuntime *runtime, int screen_width, int screen_height)
{
    return MapTileRangeForView(
        runtime,
        runtime->camera.target,
        runtime->camera.zoom,
        screen_width,
        screen_height);
}

static void RequestMapTilesInRange(MapTileCache *cache, MapTileRange range)
{
    if (!range.valid) {
        return;
    }

    for (int tile_y = range.min_y; tile_y <= range.max_y; ++tile_y) {
        for (int tile_x = range.min_x; tile_x <= range.max_x; ++tile_x) {
            GetMapTileSlot(cache, tile_x, tile_y);
        }
    }
}

static bool IsLastPageForCurrentLocation(const TravelRuntime *runtime)
{
    const TravelLocation *location = CurrentLocation(runtime);
    return (runtime->page_index + 1) >= PageCountForLocation(location);
}

static void UpdateVisibleMapTiles(TravelRuntime *runtime, int screen_width, int screen_height)
{
    MapTileCache *cache = &runtime->tiles;
    const float tile_alpha = SmoothStep01((runtime->camera.zoom - TRAVELPI_MAP_TILE_MIN_ZOOM)/0.65f);
    const MapTileRange range = VisibleMapTileRange(runtime, screen_width, screen_height);

    if (!cache->enabled) {
        return;
    }

    cache->frame_id++;
    cache->loads_this_frame = 0;

    if (range.valid && tile_alpha > 0.001f) {
        RequestMapTilesInRange(cache, range);
    }

    if (runtime->phase == TRIP_PHOTOS_OUT && IsLastPageForCurrentLocation(runtime)) {
        const TravelLocation *next_location = &g_locations[(runtime->location_index + 1) % g_location_count];
        const Vector2 next_target = ProjectGeoToMap(next_location, runtime->map.texture);
        const MapTileRange next_range = MapTileRangeForView(
            runtime,
            next_target,
            next_location->close_zoom,
            screen_width,
            screen_height);

        RequestMapTilesInRange(cache, next_range);
    }
}

static void DrawVisibleMapTiles(const TravelRuntime *runtime, int screen_width, int screen_height)
{
    MapTileCache *cache = (MapTileCache *)&runtime->tiles;
    const float tile_alpha = SmoothStep01((runtime->camera.zoom - TRAVELPI_MAP_TILE_MIN_ZOOM)/0.65f);
    const MapTileRange range = VisibleMapTileRange(runtime, screen_width, screen_height);

    if (!range.valid || tile_alpha <= 0.001f) {
        return;
    }

    for (int tile_y = range.min_y; tile_y <= range.max_y; ++tile_y) {
        for (int tile_x = range.min_x; tile_x <= range.max_x; ++tile_x) {
            MapTileSlot *slot = FindMapTileSlot(cache, tile_x, tile_y);

            if (slot == NULL || !slot->texture.loaded || slot->texture.texture.id == 0) {
                continue;
            }

            const float source_x = (float)(tile_x*cache->tile_size);
            const float source_y = (float)(tile_y*cache->tile_size);
            const float source_width = (float)slot->texture.texture.width;
            const float source_height = (float)slot->texture.texture.height;
            const float bleed = 0.75f;
            const Rectangle dest = {
                (source_x - bleed)/cache->scale_x,
                (source_y - bleed)/cache->scale_y,
                (source_width + bleed*2.0f)/cache->scale_x,
                (source_height + bleed*2.0f)/cache->scale_y,
            };

            DrawTexturePro(
                slot->texture.texture,
                (Rectangle) { 0.0f, 0.0f, source_width, source_height },
                dest,
                (Vector2) { 0.0f, 0.0f },
                0.0f,
                Fade(WHITE, tile_alpha));
        }
    }
}

static void DrawMap(TravelRuntime *runtime, int screen_width, int screen_height)
{
    Camera2D camera = { 0 };
    camera.offset = (Vector2) { (float)screen_width*0.5f, (float)screen_height*0.5f };
    camera.target = runtime->camera.target;
    camera.rotation = 0.0f;
    camera.zoom = runtime->camera.zoom;

    BeginMode2D(camera);
        DrawTexture(runtime->map.texture, 0, 0, WHITE);
        DrawVisibleMapTiles(runtime, screen_width, screen_height);
    EndMode2D();
    DrawRectangle(0, 0, screen_width, screen_height, Fade((Color) { 185, 139, 75, 255 }, 0.14f));
    DrawPaperTexture(runtime, (Rectangle) { 0.0f, 0.0f, (float)screen_width, (float)screen_height }, 0.08f);

    const bool photos_visible = (runtime->photo_alpha > 0.001f) &&
        (runtime->phase != TRIP_ZOOM_OUT);

    for (size_t i = 0; i < g_location_count; ++i) {
        const TravelLocation *location = &g_locations[i];
        const Vector2 map_pixel = ProjectGeoToMap(location, runtime->map.texture);
        const Vector2 screen_pixel = GetWorldToScreen2D(map_pixel, camera);
        const bool current = (i == runtime->location_index);
        const float radius = photos_visible ? 3.0f : (current ? 4.0f : 3.0f);
        const float alpha = photos_visible ? 0.42f : 1.0f;
        const Color fill = current ? (Color) { 183, 82, 49, 255 } : (Color) { 70, 94, 75, 210 };

        if (current) {
            const float marker_alpha = 0.98f;
            const float outline_alpha = photos_visible ? 0.96f : 1.0f;
            const float glow_radius = photos_visible ? 14.0f : 16.0f;
            const float outline_outer_radius = photos_visible ? 14.0f : 16.0f;
            const float outline_inner_radius = outline_outer_radius*0.42f;
            const float outer_radius = photos_visible ? 12.0f : 14.0f;
            const float inner_radius = outer_radius*0.42f;
            Vector2 outline_points[12];
            Vector2 star_points[12];

            outline_points[0] = screen_pixel;
            for (int point = 0; point < 10; ++point) {
                const float angle = (-90.0f + (float)point*36.0f)*DEG2RAD;
                const float outline_point_radius = ((point % 2) == 0) ? outline_outer_radius : outline_inner_radius;
                outline_points[point + 1] = (Vector2) {
                    screen_pixel.x + cosf(angle)*outline_point_radius,
                    screen_pixel.y + sinf(angle)*outline_point_radius,
                };
            }
            outline_points[11] = outline_points[1];

            star_points[0] = screen_pixel;
            for (int point = 0; point < 10; ++point) {
                const float angle = (-90.0f + (float)point*36.0f)*DEG2RAD;
                const float point_radius = ((point % 2) == 0) ? outer_radius : inner_radius;
                star_points[point + 1] = (Vector2) {
                    screen_pixel.x + cosf(angle)*point_radius,
                    screen_pixel.y + sinf(angle)*point_radius,
                };
            }
            star_points[11] = star_points[1];

            DrawCircleV(screen_pixel, glow_radius, Fade((Color) { 49, 39, 27, 255 }, photos_visible ? 0.20f : 0.28f));
            DrawTriangleFan(outline_points, 12, Fade((Color) { 54, 38, 20, 255 }, outline_alpha));
            DrawTriangleFan(star_points, 12, Fade((Color) { 242, 195, 72, 255 }, marker_alpha));
            for (int point = 1; point < 11; ++point) {
                DrawLineV(star_points[point], star_points[point + 1], Fade((Color) { 54, 38, 20, 255 }, outline_alpha*0.92f));
            }
            DrawCircleLines((int)screen_pixel.x, (int)screen_pixel.y, inner_radius + 1.0f, Fade((Color) { 255, 251, 228, 255 }, 0.92f));
        } else {
            DrawCircleV(screen_pixel, radius + 2.0f, Fade((Color) { 49, 39, 27, 255 }, alpha*0.24f));
            DrawCircleV(screen_pixel, radius, Fade(fill, alpha));
            DrawCircleLines((int)screen_pixel.x, (int)screen_pixel.y, radius + 1.0f, Fade((Color) { 42, 38, 28, 255 }, alpha*0.72f));
        }
    }
}

static void DrawPhotoCollage(const TravelRuntime *runtime, int screen_width, int screen_height)
{
    const float alpha = Clamp01(runtime->photo_alpha);

    if (alpha <= 0.001f) {
        return;
    }

    const int count = runtime->current_bank.target_count;
    const float bottom_band_height = BottomBandHeight(screen_height);
    const float rail = (float)MinFloat((float)screen_width, (float)screen_height)*0.026f;
    const float gap = MaxFloat((float)screen_width*0.018f, 22.0f);
    const float top = rail + gap*0.38f;
    const float left = rail + gap*0.42f;
    const float right = (float)screen_width - rail - gap*0.42f;
    const float bottom = (float)screen_height - bottom_band_height - gap*0.46f;
    const float width = right - left;
    const float height = bottom - top;
    const Rectangle grid_area = { left, top, width, height };
    Rectangle cells[TRAVELPI_MAX_PHOTOS_PER_LOCATION] = { 0 };

    if (count <= 0 || width <= 1.0f || height <= 1.0f) {
        return;
    }

    BuildPhotoGrid(cells, count, grid_area, gap);

    for (int i = 0; i < runtime->current_bank.target_count; ++i) {
        const TextureSlot *slot = &runtime->current_bank.photos[i];

        if (!slot->loaded || (slot->texture.id == 0)) {
            continue;
        }

        const Rectangle cell = cells[i];
        const float cell_padding = MaxFloat(MinFloat(cell.width, cell.height)*0.012f, 6.0f);
        const float mat = MaxFloat(MinFloat(cell.width, cell.height)*0.015f, 8.0f);
        const Rectangle photo_area = InsetRectangle(cell, cell_padding + mat);
        const float src_aspect = (float)slot->texture.width/(float)slot->texture.height;
        const float dst_aspect = photo_area.width/photo_area.height;
        float dst_width = photo_area.width;
        float dst_height = photo_area.height;
        Vector2 dst_pos = { photo_area.x, photo_area.y };
        Rectangle card;
        Rectangle src = { 0.0f, 0.0f, (float)slot->texture.width, (float)slot->texture.height };

        if (src_aspect > dst_aspect) {
            dst_height = photo_area.width/src_aspect;
            dst_pos.y = photo_area.y + (photo_area.height - dst_height)*0.5f;
        } else {
            dst_width = photo_area.height*src_aspect;
            dst_pos.x = photo_area.x + (photo_area.width - dst_width)*0.5f;
        }

        card = (Rectangle) {
            dst_pos.x - mat,
            dst_pos.y - mat,
            dst_width + mat*2.0f,
            dst_height + mat*2.0f,
        };

        DrawRectangle((int)(card.x + 8.0f), (int)(card.y + 10.0f), (int)card.width, (int)card.height, Fade((Color) { 47, 39, 26, 255 }, alpha*0.24f));
        DrawRectangleRec(card, Fade((Color) { 239, 232, 203, 255 }, alpha));
        DrawTexturePro(
            slot->texture,
            src,
            (Rectangle) { dst_pos.x, dst_pos.y, dst_width, dst_height },
            (Vector2) { 0.0f, 0.0f },
            0.0f,
            Fade(WHITE, alpha));
        DrawRectangleLinesEx(card, 2.0f, Fade((Color) { 255, 250, 225, 255 }, alpha*0.72f));
    }
}

static void DrawProfileOverlay(const TravelRuntime *runtime, int screen_width)
{
    char line[96];
    const FrameProfiler *profiler = &runtime->profiler;
    const Color panel = Fade((Color) { 0, 0, 0, 255 }, 0.45f);
    const Color text = Fade(RAYWHITE, 0.88f);
    const Color accent = (profiler->frame_ms > 16.67f) ? (Color) { 255, 147, 110, 255 } : (Color) { 169, 230, 180, 255 };
    const int x = 24;
    const int y = 24;
    const int width = 300;

    (void)screen_width;
    DrawRectangle(x - 10, y - 8, width, 86, panel);
    snprintf(line, sizeof(line), "frame %5.2f ms  avg %5.2f", profiler->frame_ms, profiler->avg_ms);
    DrawText(line, x, y, 16, accent);
    snprintf(line, sizeof(line), "update %5.2f ms  draw %5.2f", profiler->update_ms, profiler->draw_ms);
    DrawText(line, x, y + 23, 16, text);
    snprintf(line, sizeof(line), "worst %5.2f ms  budget 16.67", profiler->worst_ms);
    DrawText(line, x, y + 46, 16, text);
}

static void DrawOverlay(const TravelRuntime *runtime, int screen_width, int screen_height, const AppOptions *options)
{
    const TravelLocation *location = CurrentLocation(runtime);
    char page_label[32];
    const int page_count = PageCountForLocation(location);
    const float band_height = BottomBandHeight(screen_height);
    const int title_size = (int)MaxFloat((float)screen_height*0.035f, 38.0f);
    const int page_size = (int)MaxFloat((float)screen_height*0.029f, 30.0f);
    const int side_padding = (int)MaxFloat((float)screen_width*0.036f, 34.0f);
    const int band_y = (int)((float)screen_height - band_height);
    const float right_edge = (float)screen_width - (float)side_padding;
    const Vector2 title_metrics = MeasureBannerText(runtime, location->name, (float)title_size);
    const float title_y = (float)band_y + (band_height - title_metrics.y)*0.5f;

    page_label[0] = '\0';
    if (page_count > 1) {
        snprintf(page_label, sizeof(page_label), "page %d/%d", runtime->page_index + 1, page_count);
    }

    if (runtime->phase != TRIP_ZOOM_IN) {
        const Rectangle band = { 0.0f, (float)band_y, (float)screen_width, band_height + 2.0f };
        const Color ink = { 34, 39, 31, 255 };

        DrawRectangleGradientV(0, band_y - 32, screen_width, 32, (Color) { 0, 0, 0, 0 }, (Color) { 55, 40, 22, 94 });
        DrawRectangleRec(band, (Color) { 198, 175, 124, 250 });
        DrawPaperTexture(runtime, band, 0.66f);
        DrawRectangleGradientV(0, band_y, screen_width, 18, (Color) { 83, 59, 31, 90 }, (Color) { 0, 0, 0, 0 });
        DrawLine(0, band_y, screen_width, band_y, Fade((Color) { 68, 48, 27, 255 }, 0.62f));
        DrawBannerTextBold(runtime, location->name, (Vector2) { (float)side_padding, title_y }, (float)title_size, ink);

        if (page_label[0] != '\0') {
            const Vector2 page_metrics = MeasureBannerText(runtime, page_label, (float)page_size);
            const float page_y = (float)band_y + (band_height - page_metrics.y)*0.5f;
            DrawBannerTextBoldRightAligned(runtime, page_label, right_edge, page_y, (float)page_size, Fade(ink, 0.92f));
        }
    }

    if (options->show_profile) {
        DrawProfileOverlay(runtime, screen_width);
    }

    if (options->show_fps) {
        DrawFPS(screen_width - 96, 18);
    }
}

static void ParseArgs(AppOptions *options, int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--windowed") == 0) {
            options->fullscreen = false;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            options->fullscreen = true;
        } else if (strcmp(argv[i], "--show-fps") == 0) {
            options->show_fps = true;
        } else if (strcmp(argv[i], "--profile") == 0) {
            options->show_profile = true;
        } else if ((strcmp(argv[i], "--start") == 0) && (i + 1 < argc)) {
            options->start_name = argv[++i];
        } else if ((strcmp(argv[i], "--screenshot-after") == 0) && (i + 1 < argc)) {
            options->screenshot_after = (float)atof(argv[++i]);
        } else if ((strcmp(argv[i], "--screenshot-path") == 0) && (i + 1 < argc)) {
            options->screenshot_path = argv[++i];
        } else if ((strcmp(argv[i], "--exit-after") == 0) && (i + 1 < argc)) {
            options->exit_after = (float)atof(argv[++i]);
        } else if ((strcmp(argv[i], "--width") == 0) && (i + 1 < argc)) {
            options->width = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--height") == 0) && (i + 1 < argc)) {
            options->height = atoi(argv[++i]);
        }
    }
}

int main(int argc, char **argv)
{
    RuntimeTravelConfig runtime_config = { 0 };
    AppOptions options = {
        .width = TRAVELPI_DEFAULT_SCREEN_WIDTH,
        .height = TRAVELPI_DEFAULT_SCREEN_HEIGHT,
        .fullscreen = true,
        .show_fps = false,
        .show_profile = false,
        .start_name = NULL,
        .screenshot_after = 0.0f,
        .exit_after = 0.0f,
        .screenshot_path = "travelpi_screenshot.png",
    };
    ParseArgs(&options, argc, argv);

    g_locations = TRAVELPI_LOCATIONS;
    g_location_count = TRAVELPI_LOCATION_COUNT;

    if (LoadRuntimeTravelConfig(TRAVELPI_TRIPS_CONFIG_PATH, &runtime_config)) {
        g_locations = runtime_config.locations;
        g_location_count = runtime_config.location_count;
        fprintf(stderr, "travelpi: loaded trips from %s\n", TRAVELPI_TRIPS_CONFIG_PATH);
    } else {
        fprintf(stderr, "travelpi: using generated trip config fallback\n");
    }

    if (g_locations == NULL || g_location_count == 0) {
        fprintf(stderr, "travelpi: no trips configured\n");
        UnloadRuntimeTravelConfig(&runtime_config);
        return 1;
    }

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_VSYNC_HINT);

    if (options.fullscreen) {
        SetConfigFlags(FLAG_VSYNC_HINT | FLAG_FULLSCREEN_MODE);
    }

    InitWindow(options.width, options.height, "travelpi");
    SetTargetFPS(TRAVELPI_TARGET_FPS);

    const int screen_width = GetScreenWidth();
    const int screen_height = GetScreenHeight();

    TravelRuntime runtime = { 0 };
    ResetPhotoBank(&runtime.current_bank);
    runtime.banner_font_loaded = LoadBannerFont(&runtime.banner_font);
    if (!runtime.banner_font_loaded) {
        fprintf(stderr, "travelpi: banner font not found, using default font\n");
    }
    runtime.map = LoadMapTexture();

    if (!runtime.map.loaded) {
        CloseWindow();
        fprintf(stderr, "travelpi: failed to create map texture\n");
        UnloadRuntimeTravelConfig(&runtime_config);
        return 1;
    }

    runtime.paper = CreatePaperTexture();
    LoadMapTileMetadata(&runtime.tiles, runtime.map.texture);

    runtime.macro_target = (Vector2) { (float)runtime.map.texture.width*0.5f, (float)runtime.map.texture.height*0.5f };
    runtime.macro_zoom = FitMapZoom(runtime.map.texture, screen_width, screen_height);
    runtime.camera.target = runtime.macro_target;
    runtime.camera.zoom = runtime.macro_zoom;

    BeginLocation(&runtime, FindLocationIndex(options.start_name));
    bool screenshot_taken = false;
    float app_time = 0.0f;
    float config_check_time = 0.0f;
    time_t config_mtime = FileModifiedTime(TRAVELPI_TRIPS_CONFIG_PATH);

    while (!WindowShouldClose()) {
        const double frame_start = GetTime();
        const double update_start = frame_start;
        float dt = GetFrameTime();
        if (dt <= 0.0f) dt = 1.0f/(float)TRAVELPI_TARGET_FPS;
        if (dt > 0.050f) dt = 0.050f;
        app_time += dt;
        config_check_time += dt;

        if (config_check_time >= 2.0f) {
            const time_t next_mtime = FileModifiedTime(TRAVELPI_TRIPS_CONFIG_PATH);
            config_check_time = 0.0f;

            if (next_mtime != config_mtime) {
                if (ReloadRuntimeTrips(&runtime, &runtime_config, options.start_name)) {
                    config_mtime = next_mtime;
                }
            }
        }

        UpdatePhase(&runtime, dt);
        UpdateSmoothCamera(&runtime, dt);
        UpdateVisibleMapTiles(&runtime, screen_width, screen_height);
        const double draw_start = GetTime();

        BeginDrawing();
            ClearBackground((Color) { 8, 12, 16, 255 });
            DrawMap(&runtime, screen_width, screen_height);
            DrawPhotoCollage(&runtime, screen_width, screen_height);
            DrawOverlay(&runtime, screen_width, screen_height, &options);
        EndDrawing();

        if (!screenshot_taken && (options.screenshot_after > 0.0f) && (app_time >= options.screenshot_after)) {
            TakeScreenshot(options.screenshot_path);
            screenshot_taken = true;
        }

        if ((options.exit_after > 0.0f) && (app_time >= options.exit_after)) {
            break;
        }

        const double frame_end = GetTime();
        runtime.profiler.update_ms = (float)((draw_start - update_start)*1000.0);
        runtime.profiler.draw_ms = (float)((frame_end - draw_start)*1000.0);
        runtime.profiler.frame_ms = (float)((frame_end - frame_start)*1000.0);
        runtime.profiler.accumulator_ms += runtime.profiler.frame_ms;
        runtime.profiler.sample_count++;
        if (runtime.profiler.frame_ms > runtime.profiler.worst_ms) {
            runtime.profiler.worst_ms = runtime.profiler.frame_ms;
        }
        if (runtime.profiler.sample_count >= 60) {
            runtime.profiler.avg_ms = runtime.profiler.accumulator_ms/(float)runtime.profiler.sample_count;
            runtime.profiler.accumulator_ms = 0.0f;
            runtime.profiler.sample_count = 0;
            runtime.profiler.worst_ms = runtime.profiler.frame_ms;
        }
    }

    UnloadPhotoBank(&runtime.current_bank);
    UnloadMapTileCache(&runtime.tiles);
    if (runtime.banner_font_loaded) {
        UnloadFont(runtime.banner_font);
    }
    UnloadTextureSlot(&runtime.paper);
    UnloadTextureSlot(&runtime.map);
    CloseWindow();
    UnloadRuntimeTravelConfig(&runtime_config);
    return 0;
}
