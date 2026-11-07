#ifndef TEST_CJSON_H
#define TEST_CJSON_H
/* Declarations for compiling mqtt_publish.c and mqtt_discovery.c. JSON-using
 * functions are removed by dead-code stripping in the publisher test, which
 * mocks no JSON. test_mqtt_discovery.c implements the ones it reaches. */
typedef struct cJSON cJSON;
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateString(const char *);
int cJSON_AddItemToArray(cJSON *, cJSON *);
int cJSON_AddItemToObject(cJSON *, const char *, cJSON *);
cJSON *cJSON_AddStringToObject(cJSON *, const char *, const char *);
cJSON *cJSON_AddBoolToObject(cJSON *, const char *, int);
cJSON *cJSON_AddNumberToObject(cJSON *, const char *, double);
cJSON *cJSON_AddNullToObject(cJSON *, const char *);
char *cJSON_PrintUnformatted(const cJSON *);
void cJSON_Delete(cJSON *);
void cJSON_free(void *);
#endif
