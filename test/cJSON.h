#ifndef TEST_CJSON_H
#define TEST_CJSON_H
/* Declarations for compiling mqtt_publish.c. JSON-using functions are removed
 * by --gc-sections in this publisher test; no JSON implementation is mocked. */
typedef struct cJSON cJSON;
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_AddStringToObject(cJSON *, const char *, const char *);
cJSON *cJSON_AddBoolToObject(cJSON *, const char *, int);
cJSON *cJSON_AddNumberToObject(cJSON *, const char *, double);
cJSON *cJSON_AddNullToObject(cJSON *, const char *);
char *cJSON_PrintUnformatted(const cJSON *);
void cJSON_Delete(cJSON *);
#endif
