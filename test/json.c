
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>


static JSON_VALUE*
json_alloc_value(JSON_TYPE type)
{
    JSON_VALUE* v = (JSON_VALUE*) malloc(sizeof(JSON_VALUE));
    memset(v, 0, sizeof(JSON_VALUE));
    v->type = type;
    return v;
}

typedef struct JSON_BUFFER {
    char* buf;
    size_t n;
    size_t alloced;
} JSON_BUFFER;

static void
json_append(JSON_BUFFER* buf, char ch)
{
    if(buf->n >= buf->alloced) {
        if(buf->alloced > 0)
            buf->alloced *= 2;
        else
            buf->alloced = 8;

        buf->buf = realloc(buf->buf, buf->alloced);
    }

    buf->buf[buf->n] = ch;
    buf->n++;
}

static off_t
json_create_string(const char* input, off_t off, char** p_str)
{
    JSON_BUFFER buf = { 0 };

    off++;  /* skip quot */

    while(input[off] != '\"') {
        if(input[off] == '\\') {
            off++;
            switch(input[off]) {
                case 'n':   json_append(&buf, '\n'); break;
                case 'r':   json_append(&buf, '\r'); break;
                case 't':   json_append(&buf, '\t'); break;
                case '\\':  json_append(&buf, '\\'); break;
            }
            off++;
        } else {
            json_append(&buf, input[off]);
            off++;
        }
    }
    json_append(&buf, '\0');

    off++;  /* skip quot */
    *p_str = buf.buf;
    return off;
}

static off_t
json_create_unqoted_string(const char* input, off_t off, char** p_str)
{
    JSON_BUFFER buf = { 0 };

    while(strchr(" \t\r\n]}", input[off]) == NULL) {
        json_append(&buf, input[off]);
        off++;
    }
    json_append(&buf, '\0');

    *p_str = buf.buf;
    return off;
}

static void
json_add_value(JSON_VALUE* parent, char* key, JSON_VALUE* v)
{
    if(key != NULL  &&  parent->type == JSON_OBJECT) {
        if(parent->data.obj.n >= parent->data.obj.alloced) {
            if(parent->data.obj.alloced > 0)
                parent->data.obj.alloced *= 2;
            else
                parent->data.obj.alloced = 4;
            parent->data.obj.keys = realloc(parent->data.obj.keys, sizeof(char*) * parent->data.obj.alloced);
            parent->data.obj.values = realloc(parent->data.obj.values, sizeof(JSON_VALUE) * parent->data.obj.alloced);
        }

        parent->data.obj.keys[parent->data.obj.n] = key;
        parent->data.obj.values[parent->data.obj.n] = v;
        parent->data.obj.n++;
    } else if(key == NULL  &&  parent->type == JSON_ARRAY) {
        if(parent->data.array.n >= parent->data.array.alloced) {
            if(parent->data.array.alloced > 0)
                parent->data.array.alloced *= 2;
            else
                parent->data.array.alloced = 4;
            parent->data.array.values = realloc(parent->data.array.values, sizeof(JSON_VALUE) * parent->data.array.alloced);
        }

        parent->data.array.values[parent->data.array.n] = v;
        parent->data.array.n++;
    }
}

JSON_VALUE*
json_parse(const char* input)
{
    off_t off = 0;
    size_t len;
    JSON_VALUE* stack[8];
    int stack_top = 0;

    len = strlen(input);

    while(off < len) {
        char* key = NULL;
        JSON_VALUE* v = NULL;

after_key:
        while(strchr(" \t\r\n", input[off]) != NULL)
            off++;

        switch(input[off]) {
            case '{':
                v = json_alloc_value(JSON_OBJECT);
                off++;
                break;

            case '[':
                v = json_alloc_value(JSON_ARRAY);
                off++;
                break;

            case 'n':
                v = json_alloc_value(JSON_NULL);
                off += strlen("null");
                break;

            case 'f':
                v = json_alloc_value(JSON_FALSE);
                off += strlen("false");
                break;

            case 't':
                v = json_alloc_value(JSON_TRUE);
                off += strlen("true");
                break;

            case ']':
            case '}':
                off++;
                stack_top--;
                break;

            case ',':
                off++;
                break;

            case '"':
            {
                char* str;

                off = json_create_string(input, off, &str);

                while(strchr(" \t\r\n", input[off]) != NULL)
                    off++;
                if(input[off] == ':') {
                    off++;
                    key = str;
                    goto after_key;
                } else {
                    v = json_alloc_value(JSON_STRING);
                    v->data.str = str;
                }
                break;
            }

            default:
            {
                char* str;

                off = json_create_unqoted_string(input, off, &str);
                v = json_alloc_value(JSON_STRING);
                v->data.str = str;
                break;
            }
        }

        while(strchr(" \t\r\n", input[off]) != NULL)
            off++;

        if(v != NULL) {
            if(stack_top > 0)
                json_add_value(stack[stack_top-1], key, v);

            if(v->type == JSON_OBJECT || v->type == JSON_ARRAY) {
                stack[stack_top] = v;
                stack_top++;
            }
        }
    }

    return stack[0];
}

void
json_free(JSON_VALUE* tree)
{
    unsigned i;

    switch(tree->type) {
        case JSON_STRING:
            free(tree->data.str);
            break;

        case JSON_ARRAY:
            for(i = 0; i < tree->data.array.n; i++)
                json_free(tree->data.array.values[i]);
            free(tree->data.array.values);
            break;

        case JSON_OBJECT:
            for(i = 0; i < tree->data.obj.n; i++) {
                free(tree->data.obj.keys[i]);
                json_free(tree->data.obj.values[i]);
            }
            free(tree->data.obj.keys);
            free(tree->data.obj.values);
            break;

        default:
            /* noop */
            break;
    }

    free(tree);
}
