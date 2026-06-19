#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    PhotoBank current_bank;
    PhotoBank preload_bank;
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

static Rectangle InsetRectangle(Rectangle rect, float amount)
{
    return (Rectangle) {
        rect.x + amount,
        rect.y + amount,
        MaxFloat(rect.width - amount*2.0f, 1.0f),
        MaxFloat(rect.height - amount*2.0f, 1.0f),
    };
}

static unsigned char ColorByteFromFloat(float value)
{
    const int channel = (int)(Clamp01(value)*255.0f + 0.5f);
    return (unsigned char)channel;
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

static void ApplyPaperMapGrade(Image *image)
{
    ImageFormat(image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color *pixels = (Color *)image->data;
    const int pixel_count = image->width*image->height;

    if (pixels == NULL) {
        return;
    }

    for (int i = 0; i < pixel_count; ++i) {
        const Color source = pixels[i];
        float r = (float)source.r/255.0f;
        float g = (float)source.g/255.0f;
        float b = (float)source.b/255.0f;
        const float luminance = r*0.299f + g*0.587f + b*0.114f;
        const float desaturate = 0.50f;
        const float paper_mix = 0.12f;

        r = LerpFloat(r, luminance, desaturate);
        g = LerpFloat(g, luminance, desaturate);
        b = LerpFloat(b, luminance, desaturate);

        r = (r - 0.5f)*0.86f + 0.5f;
        g = (g - 0.5f)*0.86f + 0.5f;
        b = (b - 0.5f)*0.86f + 0.5f;

        r = LerpFloat(r, 0.90f, paper_mix)*1.05f;
        g = LerpFloat(g, 0.84f, paper_mix)*1.00f;
        b = LerpFloat(b, 0.68f, paper_mix)*0.88f;

        pixels[i] = (Color) {
            ColorByteFromFloat(r),
            ColorByteFromFloat(g),
            ColorByteFromFloat(b),
            source.a,
        };
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
            (Color) { 226, 214, 178, 255 },
            (Color) { 215, 199, 160, 255 });
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
                ColorByteFromInt(226 + value),
                ColorByteFromInt(214 + value),
                ColorByteFromInt(178 + value/2),
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

static TextureSlot LoadMapTexture(void)
{
    TextureSlot slot = { 0 };
    const char *path = AssetPath(TRAVELPI_MAP_PATH);
    const char *fallback_path = AssetPath(TRAVELPI_MAP_FALLBACK_PATH);

    if (FileExists(path)) {
        Image image = LoadImage(path);
        ApplyPaperMapGrade(&image);
        slot.texture = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureFilter(slot.texture, TEXTURE_FILTER_BILINEAR);
    } else if (FileExists(fallback_path)) {
        Image image = LoadImage(fallback_path);
        ApplyPaperMapGrade(&image);
        slot.texture = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureFilter(slot.texture, TEXTURE_FILTER_BILINEAR);
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

static TextureSlot LoadPhotoTexture(const char *relative_path, int fallback_index)
{
    TextureSlot slot = { 0 };
    const char *path = AssetPath(relative_path);

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
    return &TRAVELPI_LOCATIONS[runtime->location_index % TRAVELPI_LOCATION_COUNT];
}

static size_t FindLocationIndex(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) {
        return 0;
    }

    for (size_t i = 0; i < TRAVELPI_LOCATION_COUNT; ++i) {
        if (strcmp(TRAVELPI_LOCATIONS[i].name, name) == 0) {
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
    const TravelLocation *location = &TRAVELPI_LOCATIONS[location_index % TRAVELPI_LOCATION_COUNT];
    UnloadPhotoBank(bank);
    bank->location_index = location_index % TRAVELPI_LOCATION_COUNT;
    bank->page_index = page_index;
    bank->target_count = PhotoCountForPage(location, page_index);

    for (int i = 0; i < bank->target_count; ++i) {
        const int photo_index = PhotoStartForPage(page_index) + i;
        bank->photos[i] = LoadPhotoTexture(location->photos[photo_index].path, i);
        bank->cursor = i + 1;
    }

    bank->valid = true;
}

static bool IsPhotoBankComplete(const PhotoBank *bank, size_t location_index, int page_index)
{
    return bank->valid &&
        (bank->location_index == (location_index % TRAVELPI_LOCATION_COUNT)) &&
        (bank->page_index == page_index) &&
        (bank->cursor >= bank->target_count);
}

static void StartPhotoPreload(PhotoBank *bank, size_t location_index, int page_index)
{
    location_index %= TRAVELPI_LOCATION_COUNT;

    if (bank->valid && (bank->location_index == location_index) && (bank->page_index == page_index)) {
        return;
    }

    UnloadPhotoBank(bank);
    bank->location_index = location_index;
    bank->page_index = page_index;
    bank->target_count = PhotoCountForPage(&TRAVELPI_LOCATIONS[location_index], page_index);
    bank->cursor = 0;
    bank->valid = true;
}

static void ContinuePhotoPreload(PhotoBank *bank)
{
    if (!bank->valid || (bank->cursor >= bank->target_count)) {
        return;
    }

    const TravelLocation *location = &TRAVELPI_LOCATIONS[bank->location_index];
    const int cursor = bank->cursor;

    if (cursor < TRAVELPI_MAX_PHOTOS_PER_LOCATION) {
        const int photo_index = PhotoStartForPage(bank->page_index) + cursor;
        bank->photos[cursor] = LoadPhotoTexture(location->photos[photo_index].path, cursor);
    }

    bank->cursor++;
}

static void NextPageOrLocation(const TravelRuntime *runtime, size_t *location_index, int *page_index)
{
    const TravelLocation *location = CurrentLocation(runtime);
    const int page_count = PageCountForLocation(location);

    if ((runtime->page_index + 1) < page_count) {
        *location_index = runtime->location_index;
        *page_index = runtime->page_index + 1;
    } else {
        *location_index = (runtime->location_index + 1) % TRAVELPI_LOCATION_COUNT;
        *page_index = 0;
    }
}

static void PreloadNextPagePhotos(TravelRuntime *runtime)
{
    size_t next_index = 0;
    int next_page = 0;
    NextPageOrLocation(runtime, &next_index, &next_page);
    StartPhotoPreload(&runtime->preload_bank, next_index, next_page);
    ContinuePhotoPreload(&runtime->preload_bank);
}

static void BeginPage(TravelRuntime *runtime, size_t location_index, int page_index)
{
    location_index %= TRAVELPI_LOCATION_COUNT;
    runtime->location_index = location_index;
    runtime->page_index = page_index;
    runtime->phase = TRIP_ZOOM_IN;
    runtime->phase_time = 0.0f;
    runtime->photo_alpha = 0.0f;

    if (IsPhotoBankComplete(&runtime->preload_bank, location_index, page_index)) {
        UnloadPhotoBank(&runtime->current_bank);
        runtime->current_bank = runtime->preload_bank;
        ResetPhotoBank(&runtime->preload_bank);
    } else {
        LoadPhotoBankBlocking(&runtime->current_bank, location_index, page_index);
        UnloadPhotoBank(&runtime->preload_bank);
    }
}

static void BeginLocation(TravelRuntime *runtime, size_t location_index)
{
    BeginPage(runtime, location_index, 0);
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
            PreloadNextPagePhotos(runtime);
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
            PreloadNextPagePhotos(runtime);
            if (runtime->phase_time >= location->zoom_out_seconds) {
                BeginLocation(runtime, runtime->location_index + 1);
            }
            break;
    }
}

static void UpdateSmoothCamera(TravelRuntime *runtime, float dt)
{
    const TravelLocation *location = CurrentLocation(runtime);
    const bool zooming_out = (runtime->phase == TRIP_ZOOM_OUT);
    const Vector2 destination_pixel = ProjectGeoToMap(location, runtime->map.texture);
    const Vector2 desired_target = zooming_out ? runtime->macro_target : destination_pixel;
    const float desired_zoom = zooming_out ? runtime->macro_zoom : location->close_zoom;
    const float camera_blend = 1.0f - expf(-TRAVELPI_CAMERA_RESPONSE*dt);
    const float zoom_blend = 1.0f - expf(-TRAVELPI_ZOOM_RESPONSE*dt);

    runtime->camera.target = LerpVector2(runtime->camera.target, desired_target, camera_blend);
    runtime->camera.zoom = LerpFloat(runtime->camera.zoom, desired_zoom, zoom_blend);
}

static void DrawPaperTexture(const TravelRuntime *runtime, Rectangle area, float alpha)
{
    if (!runtime->paper.loaded || (runtime->paper.texture.id == 0)) {
        DrawRectangleRec(area, Fade((Color) { 226, 214, 178, 255 }, alpha));
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

static void DrawMap(const TravelRuntime *runtime, int screen_width, int screen_height)
{
    Camera2D camera = { 0 };
    camera.offset = (Vector2) { (float)screen_width*0.5f, (float)screen_height*0.5f };
    camera.target = runtime->camera.target;
    camera.rotation = 0.0f;
    camera.zoom = runtime->camera.zoom;

    BeginMode2D(camera);
        DrawTexture(runtime->map.texture, 0, 0, WHITE);
    EndMode2D();
    DrawRectangle(0, 0, screen_width, screen_height, Fade((Color) { 241, 229, 190, 255 }, 0.08f));
    DrawPaperTexture(runtime, (Rectangle) { 0.0f, 0.0f, (float)screen_width, (float)screen_height }, 0.10f);

    const bool photos_visible = (runtime->photo_alpha > 0.001f) &&
        (runtime->phase != TRIP_ZOOM_OUT);

    for (size_t i = 0; i < TRAVELPI_LOCATION_COUNT; ++i) {
        const TravelLocation *location = &TRAVELPI_LOCATIONS[i];
        const Vector2 map_pixel = ProjectGeoToMap(location, runtime->map.texture);
        const Vector2 screen_pixel = GetWorldToScreen2D(map_pixel, camera);
        const bool current = (i == runtime->location_index);
        const float radius = photos_visible ? 3.0f : (current ? 4.0f : 3.0f);
        const float alpha = photos_visible ? 0.42f : 1.0f;
        const Color fill = current ? (Color) { 183, 82, 49, 255 } : (Color) { 70, 94, 75, 210 };

        DrawCircleV(screen_pixel, radius + 2.0f, Fade((Color) { 49, 39, 27, 255 }, alpha*0.24f));
        DrawCircleV(screen_pixel, radius, Fade(fill, alpha));
        DrawCircleLines((int)screen_pixel.x, (int)screen_pixel.y, radius + 1.0f, Fade((Color) { 42, 38, 28, 255 }, alpha*0.72f));
    }
}

static void DrawPhotoCollage(const TravelRuntime *runtime, int screen_width, int screen_height)
{
    const float alpha = Clamp01(runtime->photo_alpha);

    if (alpha <= 0.001f) {
        return;
    }

    const int count = runtime->current_bank.target_count;
    const float bottom_band_height = MaxFloat((float)screen_height*0.14f, 128.0f);
    const float rail = (float)MinFloat((float)screen_width, (float)screen_height)*0.035f;
    const float gap = MaxFloat((float)screen_width*0.026f, 30.0f);
    const float top = rail + gap*0.70f;
    const float left = rail + gap*0.70f;
    const float right = (float)screen_width - rail - gap*0.70f;
    const float bottom = (float)screen_height - bottom_band_height - gap*0.95f;
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
    char caption[128];
    const int page_count = PageCountForLocation(location);
    const float band_height = MaxFloat((float)screen_height*0.14f, 128.0f);
    const int title_size = (int)MaxFloat((float)screen_height*0.042f, 36.0f);
    const int date_size = (int)MaxFloat((float)screen_height*0.024f, 21.0f);
    const int caption_size = (int)MaxFloat((float)screen_height*0.022f, 19.0f);
    const int x = (int)MaxFloat((float)screen_width*0.036f, 34.0f);
    const int right_padding = x;
    const int band_y = (int)((float)screen_height - band_height);
    const int y = band_y + (int)MaxFloat(band_height*0.13f, 14.0f);

    if (page_count > 1) {
        snprintf(caption, sizeof(caption), "%s   page %d/%d", location->caption, runtime->page_index + 1, page_count);
    } else {
        snprintf(caption, sizeof(caption), "%s", location->caption);
    }

    const int has_date = (location->date_label != NULL) && (location->date_label[0] != '\0');

    if (runtime->phase != TRIP_ZOOM_IN) {
        const int caption_width = MeasureText(caption, caption_size);
        const int caption_x = screen_width - right_padding - caption_width;
        const int caption_y = band_y + (int)((band_height - (float)caption_size)*0.52f);
        const Rectangle band = { 0.0f, (float)band_y, (float)screen_width, band_height + 2.0f };
        const Color ink = { 38, 45, 38, 255 };
        const Color muted_ink = { 76, 84, 67, 255 };

        DrawRectangleGradientV(0, band_y - 32, screen_width, 32, (Color) { 0, 0, 0, 0 }, (Color) { 71, 57, 34, 74 });
        DrawRectangleRec(band, (Color) { 231, 220, 186, 248 });
        DrawPaperTexture(runtime, band, 0.58f);
        DrawRectangleGradientV(0, band_y, screen_width, 18, (Color) { 112, 87, 48, 76 }, (Color) { 0, 0, 0, 0 });
        DrawLine(0, band_y, screen_width, band_y, Fade((Color) { 84, 69, 43, 255 }, 0.58f));
        DrawText(location->name, x, y, title_size, ink);

        if (has_date) {
            DrawText(location->date_label, x, y + title_size + 4, date_size, Fade(muted_ink, 0.92f));
            DrawText(caption, caption_x, caption_y, caption_size, Fade(ink, 0.88f));
        } else {
            DrawText(caption, caption_x, caption_y, caption_size, Fade(ink, 0.88f));
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
    ResetPhotoBank(&runtime.preload_bank);
    runtime.map = LoadMapTexture();

    if (!runtime.map.loaded) {
        CloseWindow();
        fprintf(stderr, "travelpi: failed to create map texture\n");
        return 1;
    }

    runtime.paper = CreatePaperTexture();

    runtime.macro_target = (Vector2) { (float)runtime.map.texture.width*0.5f, (float)runtime.map.texture.height*0.5f };
    runtime.macro_zoom = FitMapZoom(runtime.map.texture, screen_width, screen_height);
    runtime.camera.target = runtime.macro_target;
    runtime.camera.zoom = runtime.macro_zoom;

    BeginLocation(&runtime, FindLocationIndex(options.start_name));
    bool screenshot_taken = false;
    float app_time = 0.0f;

    while (!WindowShouldClose()) {
        const double frame_start = GetTime();
        const double update_start = frame_start;
        float dt = GetFrameTime();
        if (dt <= 0.0f) dt = 1.0f/(float)TRAVELPI_TARGET_FPS;
        if (dt > 0.050f) dt = 0.050f;
        app_time += dt;

        UpdatePhase(&runtime, dt);
        UpdateSmoothCamera(&runtime, dt);
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
    UnloadPhotoBank(&runtime.preload_bank);
    UnloadTextureSlot(&runtime.paper);
    UnloadTextureSlot(&runtime.map);
    CloseWindow();
    return 0;
}
