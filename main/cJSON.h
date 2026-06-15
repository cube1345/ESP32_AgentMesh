#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef int cJSON_bool;

#define cJSON_Invalid        0
#define cJSON_False          (1 << 0)
#define cJSON_True           (1 << 1)
#define cJSON_NULL           (1 << 2)
#define cJSON_Number         (1 << 3)
#define cJSON_String         (1 << 4)
#define cJSON_Array          (1 << 5)
#define cJSON_Object         (1 << 6)
#define cJSON_Raw            (1 << 7)
#define cJSON_IsReference    256
#define cJSON_StringIsConst  512

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;

    int type;

    char *valuestring;
    int valueint;
    double valuedouble;

    char *string;
} cJSON;

typedef struct cJSON_Hooks {
    void *(*malloc_fn)(size_t sz);
    void (*free_fn)(void *ptr);
} cJSON_Hooks;

void cJSON_InitHooks(cJSON_Hooks *hooks);

cJSON *cJSON_Parse(const char *value);
cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length);
cJSON *cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated);
cJSON *cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated);
char *cJSON_Print(const cJSON *item);
char *cJSON_PrintUnformatted(const cJSON *item);
char *cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool format);
cJSON_bool cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format);
void cJSON_Delete(cJSON *item);

int cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
cJSON_bool cJSON_HasObjectItem(const cJSON *object, const char *string);
const char *cJSON_GetStringValue(const cJSON *item);
double cJSON_GetNumberValue(const cJSON *item);
const char *cJSON_GetErrorPtr(void);

cJSON_bool cJSON_IsInvalid(const cJSON *item);
cJSON_bool cJSON_IsFalse(const cJSON *item);
cJSON_bool cJSON_IsTrue(const cJSON *item);
cJSON_bool cJSON_IsBool(const cJSON *item);
cJSON_bool cJSON_IsNull(const cJSON *item);
cJSON_bool cJSON_IsNumber(const cJSON *item);
cJSON_bool cJSON_IsString(const cJSON *item);
cJSON_bool cJSON_IsArray(const cJSON *item);
cJSON_bool cJSON_IsObject(const cJSON *item);
cJSON_bool cJSON_IsRaw(const cJSON *item);

cJSON *cJSON_CreateNull(void);
cJSON *cJSON_CreateTrue(void);
cJSON *cJSON_CreateFalse(void);
cJSON *cJSON_CreateBool(cJSON_bool boolean);
cJSON *cJSON_CreateNumber(double num);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateRaw(const char *raw);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateObject(void);

cJSON *cJSON_CreateStringReference(const char *string);
cJSON *cJSON_CreateObjectReference(const cJSON *child);
cJSON *cJSON_CreateArrayReference(const cJSON *child);
cJSON *cJSON_CreateIntArray(const int *numbers, int count);
cJSON *cJSON_CreateFloatArray(const float *numbers, int count);
cJSON *cJSON_CreateDoubleArray(const double *numbers, int count);
cJSON *cJSON_CreateStringArray(const char *const *strings, int count);

void cJSON_AddItemToArray(cJSON *array, cJSON *item);
void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
void cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);
void cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);
void cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);

cJSON *cJSON_DetachItemViaPointer(cJSON *parent, cJSON *const item);
cJSON *cJSON_DetachItemFromArray(cJSON *array, int which);
void cJSON_DeleteItemFromArray(cJSON *array, int which);
cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string);
cJSON *cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string);
void cJSON_DeleteItemFromObject(cJSON *object, const char *string);
void cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string);

void cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem);
void cJSON_ReplaceItemViaPointer(cJSON *const parent, cJSON *const item, cJSON *replacement);
void cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);
void cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem);
void cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem);

cJSON *cJSON_Duplicate(const cJSON *item, cJSON_bool recurse);
cJSON_bool cJSON_Compare(const cJSON *a, const cJSON *b, cJSON_bool case_sensitive);
void cJSON_Minify(char *json);
char *cJSON_Version(void);

void *cJSON_malloc(size_t size);
void cJSON_free(void *object);

#define cJSON_AddNullToObject(object, name) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateNull())
#define cJSON_AddTrueToObject(object, name) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateTrue())
#define cJSON_AddFalseToObject(object, name) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateFalse())
#define cJSON_AddBoolToObject(object, name, boolean) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateBool((boolean)))
#define cJSON_AddNumberToObject(object, name, number) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateNumber((number)))
#define cJSON_AddStringToObject(object, name, string) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateString((string)))
#define cJSON_AddRawToObject(object, name, raw) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateRaw((raw)))
#define cJSON_AddObjectToObject(object, name) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateObject())
#define cJSON_AddArrayToObject(object, name) \
    cJSON_AddItemToObject((object), (name), cJSON_CreateArray())

#define cJSON_ArrayForEach(element, array) \
    for ((element) = ((array) != NULL) ? (array)->child : NULL; (element) != NULL; (element) = (element)->next)

#ifdef __cplusplus
}
#endif
