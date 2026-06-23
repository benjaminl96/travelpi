#include "travel_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum JsonType {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct JsonMember {
    char *key;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            size_t count;
        } array;
        struct {
            JsonMember *members;
            size_t count;
        } object;
    } as;
};

static const PhotoSpec DEFAULT_LAYOUT[] = {
    { NULL, { 0.30f, 0.48f }, 0.42f, -3.0f, 0.4f },
    { NULL, { 0.70f, 0.48f }, 0.42f, 3.0f, 1.5f },
};

static char *DuplicateString(const char *value)
{
    const size_t length = strlen(value);
    char *copy = (char *)malloc(length + 1);

    if (copy != NULL) {
        memcpy(copy, value, length + 1);
    }

    return copy;
}

static char *ReadTextFile(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size = 0;
    char *data = NULL;

    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }

    rewind(file);
    data = (char *)malloc((size_t)size + 1);

    if (data == NULL) {
        fclose(file);
        return NULL;
    }

    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }

    data[size] = '\0';
    fclose(file);
    return data;
}

static void SkipWhitespace(const char **cursor)
{
    while (**cursor != '\0' && isspace((unsigned char)**cursor)) {
        ++(*cursor);
    }
}

static JsonValue *NewJsonValue(JsonType type)
{
    JsonValue *value = (JsonValue *)calloc(1, sizeof(JsonValue));

    if (value != NULL) {
        value->type = type;
    }

    return value;
}

static void FreeJson(JsonValue *value)
{
    if (value == NULL) {
        return;
    }

    if (value->type == JSON_STRING) {
        free(value->as.string);
    } else if (value->type == JSON_ARRAY) {
        for (size_t i = 0; i < value->as.array.count; ++i) {
            FreeJson(value->as.array.items[i]);
        }
        free(value->as.array.items);
    } else if (value->type == JSON_OBJECT) {
        for (size_t i = 0; i < value->as.object.count; ++i) {
            free(value->as.object.members[i].key);
            FreeJson(value->as.object.members[i].value);
        }
        free(value->as.object.members);
    }

    free(value);
}

static bool AppendChar(char **buffer, size_t *length, size_t *capacity, char ch)
{
    if (*length + 1 >= *capacity) {
        const size_t next_capacity = (*capacity == 0) ? 32 : (*capacity * 2);
        char *next = (char *)realloc(*buffer, next_capacity);

        if (next == NULL) {
            return false;
        }

        *buffer = next;
        *capacity = next_capacity;
    }

    (*buffer)[(*length)++] = ch;
    (*buffer)[*length] = '\0';
    return true;
}

static char *ParseJsonStringRaw(const char **cursor)
{
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;

    if (**cursor != '"') {
        return NULL;
    }

    ++(*cursor);
    while (**cursor != '\0' && **cursor != '"') {
        char ch = *(*cursor)++;

        if (ch == '\\') {
            ch = *(*cursor)++;

            switch (ch) {
                case '"': break;
                case '\\': break;
                case '/': break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'u':
                    for (int i = 0; i < 4 && isxdigit((unsigned char)**cursor); ++i) {
                        ++(*cursor);
                    }
                    ch = '?';
                    break;
                default:
                    free(buffer);
                    return NULL;
            }
        }

        if (!AppendChar(&buffer, &length, &capacity, ch)) {
            free(buffer);
            return NULL;
        }
    }

    if (**cursor != '"') {
        free(buffer);
        return NULL;
    }

    ++(*cursor);
    if (buffer == NULL) {
        buffer = DuplicateString("");
    }

    return buffer;
}

static JsonValue *ParseJsonValue(const char **cursor);

static JsonValue *ParseJsonArray(const char **cursor)
{
    JsonValue *array = NewJsonValue(JSON_ARRAY);

    if (array == NULL || **cursor != '[') {
        FreeJson(array);
        return NULL;
    }

    ++(*cursor);
    SkipWhitespace(cursor);

    if (**cursor == ']') {
        ++(*cursor);
        return array;
    }

    while (**cursor != '\0') {
        JsonValue *item = ParseJsonValue(cursor);
        JsonValue **items = NULL;

        if (item == NULL) {
            FreeJson(array);
            return NULL;
        }

        items = (JsonValue **)realloc(array->as.array.items, sizeof(JsonValue *)*(array->as.array.count + 1));
        if (items == NULL) {
            FreeJson(item);
            FreeJson(array);
            return NULL;
        }

        array->as.array.items = items;
        array->as.array.items[array->as.array.count++] = item;
        SkipWhitespace(cursor);

        if (**cursor == ']') {
            ++(*cursor);
            return array;
        }

        if (**cursor != ',') {
            FreeJson(array);
            return NULL;
        }

        ++(*cursor);
        SkipWhitespace(cursor);
    }

    FreeJson(array);
    return NULL;
}

static JsonValue *ParseJsonObject(const char **cursor)
{
    JsonValue *object = NewJsonValue(JSON_OBJECT);

    if (object == NULL || **cursor != '{') {
        FreeJson(object);
        return NULL;
    }

    ++(*cursor);
    SkipWhitespace(cursor);

    if (**cursor == '}') {
        ++(*cursor);
        return object;
    }

    while (**cursor != '\0') {
        char *key = ParseJsonStringRaw(cursor);
        JsonMember *members = NULL;
        JsonValue *member_value = NULL;

        if (key == NULL) {
            FreeJson(object);
            return NULL;
        }

        SkipWhitespace(cursor);
        if (**cursor != ':') {
            free(key);
            FreeJson(object);
            return NULL;
        }

        ++(*cursor);
        SkipWhitespace(cursor);
        member_value = ParseJsonValue(cursor);

        if (member_value == NULL) {
            free(key);
            FreeJson(object);
            return NULL;
        }

        members = (JsonMember *)realloc(object->as.object.members, sizeof(JsonMember)*(object->as.object.count + 1));
        if (members == NULL) {
            free(key);
            FreeJson(member_value);
            FreeJson(object);
            return NULL;
        }

        object->as.object.members = members;
        object->as.object.members[object->as.object.count++] = (JsonMember) { key, member_value };
        SkipWhitespace(cursor);

        if (**cursor == '}') {
            ++(*cursor);
            return object;
        }

        if (**cursor != ',') {
            FreeJson(object);
            return NULL;
        }

        ++(*cursor);
        SkipWhitespace(cursor);
    }

    FreeJson(object);
    return NULL;
}

static JsonValue *ParseJsonValue(const char **cursor)
{
    char *end = NULL;
    JsonValue *value = NULL;

    SkipWhitespace(cursor);

    if (**cursor == '"') {
        value = NewJsonValue(JSON_STRING);
        if (value == NULL) return NULL;
        value->as.string = ParseJsonStringRaw(cursor);
        if (value->as.string == NULL) {
            FreeJson(value);
            return NULL;
        }
        return value;
    }

    if (**cursor == '[') {
        return ParseJsonArray(cursor);
    }

    if (**cursor == '{') {
        return ParseJsonObject(cursor);
    }

    if (strncmp(*cursor, "true", 4) == 0) {
        *cursor += 4;
        value = NewJsonValue(JSON_BOOL);
        if (value != NULL) value->as.boolean = true;
        return value;
    }

    if (strncmp(*cursor, "false", 5) == 0) {
        *cursor += 5;
        value = NewJsonValue(JSON_BOOL);
        if (value != NULL) value->as.boolean = false;
        return value;
    }

    if (strncmp(*cursor, "null", 4) == 0) {
        *cursor += 4;
        return NewJsonValue(JSON_NULL);
    }

    value = NewJsonValue(JSON_NUMBER);
    if (value == NULL) return NULL;
    value->as.number = strtod(*cursor, &end);

    if (end == *cursor) {
        FreeJson(value);
        return NULL;
    }

    *cursor = end;
    return value;
}

static JsonValue *ParseJson(const char *text)
{
    const char *cursor = text;
    JsonValue *root = ParseJsonValue(&cursor);

    if (root == NULL) {
        return NULL;
    }

    SkipWhitespace(&cursor);
    if (*cursor != '\0') {
        FreeJson(root);
        return NULL;
    }

    return root;
}

static const JsonValue *ObjectGet(const JsonValue *object, const char *key)
{
    if (object == NULL || object->type != JSON_OBJECT) {
        return NULL;
    }

    for (size_t i = 0; i < object->as.object.count; ++i) {
        if (strcmp(object->as.object.members[i].key, key) == 0) {
            return object->as.object.members[i].value;
        }
    }

    return NULL;
}

static const char *ObjectString(const JsonValue *object, const char *key, const char *fallback)
{
    const JsonValue *value = ObjectGet(object, key);

    if (value != NULL && value->type == JSON_STRING) {
        return value->as.string;
    }

    return fallback;
}

static float ObjectFloat(const JsonValue *object, const char *key, float fallback)
{
    const JsonValue *value = ObjectGet(object, key);

    if (value != NULL && value->type == JSON_NUMBER) {
        return (float)value->as.number;
    }

    return fallback;
}

static bool ArrayPair(const JsonValue *array, float *x, float *y)
{
    if (array == NULL || array->type != JSON_ARRAY || array->as.array.count < 2) {
        return false;
    }

    if (array->as.array.items[0]->type != JSON_NUMBER || array->as.array.items[1]->type != JSON_NUMBER) {
        return false;
    }

    *x = (float)array->as.array.items[0]->as.number;
    *y = (float)array->as.array.items[1]->as.number;
    return true;
}

static bool IsoDateParts(const char *value, int *year, int *month, int *day)
{
    if (value == NULL) {
        return false;
    }

    return sscanf(value, "%d-%d-%d", year, month, day) == 3;
}

static const char *MonthName(int month)
{
    static const char *months[] = {
        "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    if (month < 1 || month > 12) {
        return "";
    }

    return months[month];
}

static char *BuildDateLabel(const JsonValue *trip)
{
    const char *explicit_label = ObjectString(trip, "date_label", NULL);
    const char *start = ObjectString(trip, "start_date", NULL);
    const char *end = ObjectString(trip, "end_date", NULL);
    int sy = 0;
    int sm = 0;
    int sd = 0;
    int ey = 0;
    int em = 0;
    int ed = 0;
    char buffer[128];

    if (explicit_label != NULL && explicit_label[0] != '\0') {
        return DuplicateString(explicit_label);
    }

    if (!IsoDateParts(start, &sy, &sm, &sd)) {
        return DuplicateString("");
    }

    if (!IsoDateParts(end, &ey, &em, &ed) || strcmp(start, end) == 0) {
        snprintf(buffer, sizeof(buffer), "%s %d, %d", MonthName(sm), sd, sy);
    } else if (sy == ey && sm == em) {
        snprintf(buffer, sizeof(buffer), "%s %d-%d, %d", MonthName(sm), sd, ed, sy);
    } else if (sy == ey) {
        snprintf(buffer, sizeof(buffer), "%s %d - %s %d, %d", MonthName(sm), sd, MonthName(em), ed, sy);
    } else {
        snprintf(buffer, sizeof(buffer), "%s %d, %d - %s %d, %d", MonthName(sm), sd, sy, MonthName(em), ed, ey);
    }

    return DuplicateString(buffer);
}

static bool BuildPhoto(const JsonValue *photo_json, size_t index, PhotoSpec *photo)
{
    const PhotoSpec defaults = DEFAULT_LAYOUT[index % (sizeof(DEFAULT_LAYOUT)/sizeof(DEFAULT_LAYOUT[0]))];
    const char *path = ObjectString(photo_json, "path", NULL);
    float x = defaults.anchor.x;
    float y = defaults.anchor.y;

    if (path == NULL || path[0] == '\0') {
        return false;
    }

    *photo = defaults;
    photo->path = DuplicateString(path);
    if (photo->path == NULL) {
        return false;
    }

    if (ArrayPair(ObjectGet(photo_json, "anchor"), &x, &y)) {
        photo->anchor = (Vector2) { x, y };
    }

    photo->scale = ObjectFloat(photo_json, "scale", defaults.scale);
    photo->rotation = ObjectFloat(photo_json, "rotation", defaults.rotation);
    photo->drift = ObjectFloat(photo_json, "drift", defaults.drift);
    return true;
}

static void FreeLocation(TravelLocation *location)
{
    if (location == NULL) {
        return;
    }

    free((char *)location->name);
    free((char *)location->caption);
    free((char *)location->date_label);

    for (int i = 0; i < location->photo_count; ++i) {
        free((char *)location->photos[i].path);
    }

    free((PhotoSpec *)location->photos);
    memset(location, 0, sizeof(*location));
}

static bool BuildLocation(const JsonValue *trip_json, TravelLocation *location)
{
    const JsonValue *geo = ObjectGet(trip_json, "geo");
    const JsonValue *photos_json = ObjectGet(trip_json, "photos");
    const char *name = ObjectString(trip_json, "name", NULL);
    const char *caption = ObjectString(trip_json, "caption", name);
    float latitude = 0.0f;
    float longitude = 0.0f;
    float nudge_x = 0.0f;
    float nudge_y = 0.0f;

    memset(location, 0, sizeof(*location));

    if (name == NULL || name[0] == '\0' || geo == NULL || geo->type != JSON_OBJECT) {
        return false;
    }

    location->name = DuplicateString(name);
    location->caption = DuplicateString(caption != NULL ? caption : name);
    location->date_label = BuildDateLabel(trip_json);

    if (location->name == NULL || location->caption == NULL || location->date_label == NULL) {
        FreeLocation(location);
        return false;
    }

    latitude = ObjectFloat(geo, "latitude", 0.0f);
    longitude = ObjectFloat(geo, "longitude", 0.0f);
    location->geo = (GeoCoord) { latitude, longitude };

    if (ArrayPair(ObjectGet(trip_json, "pixel_nudge"), &nudge_x, &nudge_y)) {
        location->pixel_nudge = (Vector2) { nudge_x, nudge_y };
    } else {
        location->pixel_nudge = (Vector2) { 0.0f, 0.0f };
    }

    location->close_zoom = ObjectFloat(trip_json, "close_zoom", 10.0f);
    location->zoom_in_seconds = ObjectFloat(trip_json, "zoom_in_seconds", 2.35f);
    location->hold_seconds = ObjectFloat(trip_json, "hold_seconds", 15.0f);
    location->fade_seconds = ObjectFloat(trip_json, "fade_seconds", 0.45f);
    location->zoom_out_seconds = ObjectFloat(trip_json, "zoom_out_seconds", 2.05f);

    if (photos_json != NULL && photos_json->type == JSON_ARRAY && photos_json->as.array.count > 0) {
        PhotoSpec *photos = (PhotoSpec *)calloc(photos_json->as.array.count, sizeof(PhotoSpec));

        if (photos == NULL) {
            FreeLocation(location);
            return false;
        }

        for (size_t i = 0; i < photos_json->as.array.count; ++i) {
            if (photos_json->as.array.items[i]->type != JSON_OBJECT ||
                !BuildPhoto(photos_json->as.array.items[i], i, &photos[i])) {
                for (size_t j = 0; j < i; ++j) {
                    free((char *)photos[j].path);
                }
                free(photos);
                FreeLocation(location);
                return false;
            }
        }

        location->photo_count = (int)photos_json->as.array.count;
        location->photos = photos;
    }

    return true;
}

bool LoadRuntimeTravelConfig(const char *path, RuntimeTravelConfig *config)
{
    char *text = ReadTextFile(path);
    JsonValue *root = NULL;
    const JsonValue *trips = NULL;
    TravelLocation *locations = NULL;

    if (config == NULL) {
        free(text);
        return false;
    }

    config->locations = NULL;
    config->location_count = 0;

    if (text == NULL) {
        return false;
    }

    root = ParseJson(text);
    free(text);

    if (root == NULL || root->type != JSON_OBJECT) {
        FreeJson(root);
        return false;
    }

    trips = ObjectGet(root, "trips");
    if (trips == NULL || trips->type != JSON_ARRAY || trips->as.array.count == 0) {
        FreeJson(root);
        return false;
    }

    locations = (TravelLocation *)calloc(trips->as.array.count, sizeof(TravelLocation));
    if (locations == NULL) {
        FreeJson(root);
        return false;
    }

    for (size_t i = 0; i < trips->as.array.count; ++i) {
        if (trips->as.array.items[i]->type != JSON_OBJECT ||
            !BuildLocation(trips->as.array.items[i], &locations[i])) {
            for (size_t j = 0; j < i; ++j) {
                FreeLocation(&locations[j]);
            }
            free(locations);
            FreeJson(root);
            return false;
        }
    }

    config->locations = locations;
    config->location_count = trips->as.array.count;
    FreeJson(root);
    return true;
}

void UnloadRuntimeTravelConfig(RuntimeTravelConfig *config)
{
    if (config == NULL || config->locations == NULL) {
        return;
    }

    for (size_t i = 0; i < config->location_count; ++i) {
        FreeLocation(&config->locations[i]);
    }

    free(config->locations);
    config->locations = NULL;
    config->location_count = 0;
}
