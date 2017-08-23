
#ifndef TEST_JSON_H
#define TEST_JSON_H

/* Note this is very dirty implementation of JSON with very limited
 * capabilities, and error handling close to zero.
 * It is good enough just for our testing purposes.
 * Do not use for anything serious.
 * You have been warned.
 */



typedef struct JSON_VALUE JSON_VALUE;
typedef struct JSON_ARRAY_DATA JSON_ARRAY_DATA;
typedef struct JSON_OBJECT_DATA JSON_OBJECT_DATA;
typedef struct JSON_VALUE JSON_VALUE;
typedef enum JSON_TYPE JSON_TYPE;

enum JSON_TYPE {
    JSON_NULL,
    JSON_FALSE,
    JSON_TRUE,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
};

struct JSON_ARRAY_DATA {
    JSON_VALUE** values;
    unsigned alloced;
    unsigned n;
};

struct JSON_OBJECT_DATA {
    char** keys;
    JSON_VALUE** values;
    unsigned alloced;
    unsigned n;
};

struct JSON_VALUE {
    JSON_TYPE type;
    union {
        char* str;
        JSON_ARRAY_DATA array;
        JSON_OBJECT_DATA obj;
    } data;
};


JSON_VALUE* json_parse(const char* input);
void json_free(JSON_VALUE* tree);


#endif  /* TEST_JSON_H */

