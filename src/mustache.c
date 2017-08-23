/*
 * Mustache4C
 * (http://github.com/mity/mustache4c)
 *
 * Copyright (c) 2017 Martin Mitas
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "mustache.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define MUSTACHE_DEFAULTOPENER      "{{"
#define MUSTACHE_DEFAULTCLOSER      "}}"
#define MUSTACHE_MAXOPENERLENGTH    32
#define MUSTACHE_MAXCLOSERLENGTH    32


#define MUSTACHE_ISANYOF2(ch, ch1, ch2)            ((ch) == (ch1) || (ch) == (ch2))
#define MUSTACHE_ISANYOF4(ch, ch1, ch2, ch3, ch4)  ((ch) == (ch1) || (ch) == (ch2) || (ch) == (ch3) || (ch) == (ch4))

#define MUSTACHE_ISWHITESPACE(ch)   MUSTACHE_ISANYOF4((ch), ' ', '\t', '\v', '\f')
#define MUSTACHE_ISNEWLINE(ch)      MUSTACHE_ISANYOF2((ch), '\r', '\n')


typedef enum MUSTACHE_TAGTYPE {
    MUSTACHE_TAGTYPE_NONE = 0,
    MUSTACHE_TAGTYPE_COMMENT,           /* {{! comment }} */
    MUSTACHE_TAGTYPE_VAR,               /* {{ var }} */
    MUSTACHE_TAGTYPE_VERBATIMVAR,       /* {{{ var }}} */
    MUSTACHE_TAGTYPE_VERBATIMVAR2,      /* {{& var }} */
    MUSTACHE_TAGTYPE_OPENSECTION,       /* {{# section }} */
    MUSTACHE_TAGTYPE_OPENSECTIONINV,    /* {{^ section }} */
    MUSTACHE_TAGTYPE_CLOSESECTION,      /* {{/ section }} */
    MUSTACHE_TAGTYPE_PARTIAL            /* {{> partial }} */
} MUSTACHE_TAGTYPE;


typedef struct MUSTACHE_TAGINFO {
    MUSTACHE_TAGTYPE type;
    off_t line;
    off_t col;
    off_t beg;
    off_t end;
    off_t name_beg;
    off_t name_end;
} MUSTACHE_TAGINFO;


/* Keep in sync with MUSTACHE_ERR_xxx constants. */
static const char* mustache_err_messages[] = {
    "Success.",
    "Tag opener has no closer.",
    "Tag closer has no opener.",
    "Tag closer is incompatible with its opener.",
    "Tag has no name."
};



static void
mustache_sys_error(int err_code, void* parser_data)
{
    /* noop */
}

static void
mustache_parse_error(int err_code, const char* msg,
                    unsigned line, unsigned column, void* parser_data)
{
    /* noop */
}


static int
mustache_is_std_closer(const char* closer, size_t closer_len)
{
    off_t off;

    for(off = 0; off < closer_len; off++) {
        if(closer[off] != '}')
            return 0;
    }

    return 1;
}

static int
mustache_parse(const char* templ_data, size_t templ_size,
               const MUSTACHE_PARSER* parser, void* parser_data,
               MUSTACHE_TAGINFO** p_tags, unsigned* p_n_tags)
{
    int n_errors = 0;
    char opener[MUSTACHE_MAXOPENERLENGTH] = MUSTACHE_DEFAULTOPENER;
    char closer[MUSTACHE_MAXCLOSERLENGTH] = MUSTACHE_DEFAULTCLOSER;
    size_t opener_len;
    size_t closer_len;
    off_t off = 0;
    off_t line = 1;
    off_t col = 1;
    MUSTACHE_TAGINFO current_tag;
    MUSTACHE_TAGINFO* tags = NULL;
    unsigned n_tags = 0;
    unsigned n_alloced_tags = 0;

    current_tag.type = MUSTACHE_TAGTYPE_NONE;

    opener_len = strlen(MUSTACHE_DEFAULTOPENER);
    closer_len = strlen(MUSTACHE_DEFAULTCLOSER);

    while(off < templ_size) {
        int is_opener, is_closer;

        is_opener = (off + opener_len <= templ_size  &&  strncmp(templ_data+off, opener, opener_len) == 0);
        is_closer = (off + closer_len <= templ_size  &&  strncmp(templ_data+off, closer, closer_len) == 0);
        if(is_opener && is_closer) {
            /* Opener and closer may be defined to be the same string.
             * Consider for example "{{=@ @=}}".
             * Determine the real meaning from current parser state:
             */
            if(current_tag.type == MUSTACHE_TAGTYPE_NONE)
                is_closer = 0;
            else
                is_opener = 0;
        }

        if(is_opener) {
            /* Handle tag opener "{{" */

            if(current_tag.type != MUSTACHE_TAGTYPE_NONE  &&  current_tag.type != MUSTACHE_TAGTYPE_COMMENT) {
                /* Opener after some previous opener??? */
                parser->parse_error(MUSTACHE_ERR_DANGLINGTAGOPENER,
                        mustache_err_messages[MUSTACHE_ERR_DANGLINGTAGOPENER],
                        (unsigned)current_tag.line, (unsigned)current_tag.col,
                        parser_data);
                n_errors++;
                current_tag.type = MUSTACHE_TAGTYPE_NONE;
            }

            current_tag.line = line;
            current_tag.col = col;
            current_tag.beg = off;
            off += opener_len;

            if(off < templ_size) {
                switch(templ_data[off]) {
                    case '!':   current_tag.type = MUSTACHE_TAGTYPE_COMMENT; off++; break;
                    case '{':   current_tag.type = MUSTACHE_TAGTYPE_VERBATIMVAR; off++; break;
                    case '&':   current_tag.type = MUSTACHE_TAGTYPE_VERBATIMVAR2; off++; break;
                    case '#':   current_tag.type = MUSTACHE_TAGTYPE_OPENSECTION; off++; break;
                    case '^':   current_tag.type = MUSTACHE_TAGTYPE_OPENSECTIONINV; off++; break;
                    case '/':   current_tag.type = MUSTACHE_TAGTYPE_CLOSESECTION; off++; break;
                    case '>':   current_tag.type = MUSTACHE_TAGTYPE_PARTIAL; off++; break;

                    // TODO: handle delimiter reset specially
                    // (Consider strange things like "{{={{ }}=}}" which may
                    // have the same old and new opener and/or closer.)
                    //case '=':   current_tag.type = MUSTACHE_TAGTYPE_DELIM; off++; break;

                    default:    current_tag.type = MUSTACHE_TAGTYPE_VAR; break;
                }
            }

            while(off < templ_size  &&  MUSTACHE_ISWHITESPACE(templ_data[off]))
                off++;
            current_tag.name_beg = off;

            col += current_tag.name_beg - current_tag.beg;
        } else if(is_closer  &&  current_tag.type == MUSTACHE_TAGTYPE_NONE) {
            /* Invalid closer. */
            parser->parse_error(MUSTACHE_ERR_DANGLINGTAGCLOSER,
                    mustache_err_messages[MUSTACHE_ERR_DANGLINGTAGCLOSER],
                    (unsigned) line, (unsigned) col, parser_data);
            n_errors++;
            off++;
            col++;
        } else if(is_closer) {
            /* Handle tag closer "}}" */

            current_tag.name_end = off;
            off += closer_len;
            col += closer_len;
            if(current_tag.type == MUSTACHE_TAGTYPE_VERBATIMVAR) {
                /* Eat the extra '}'. Note it may be after the found
                 * closer (if closer is "}}" or before it for a custom
                 * closer. */
                if(current_tag.name_end > current_tag.name_beg  &&
                            templ_data[current_tag.name_end-1] == '}') {
                    current_tag.name_end--;
                } else if(mustache_is_std_closer(closer, closer_len)  &&
                            off < templ_size  &&  templ_data[off] == '}') {
                    off++;
                    col++;
                } else {
                    parser->parse_error(MUSTACHE_ERR_INCOMPATIBLETAGCLOSER,
                            mustache_err_messages[MUSTACHE_ERR_INCOMPATIBLETAGCLOSER],
                            (unsigned) line, (unsigned) col, parser_data);
                    n_errors++;
                }
            }

            current_tag.end = off;

            /* If the tag is standalone, expand it to consumme also any
             * preceding whitespace and also one new-line (before or after). */
            if(current_tag.type != MUSTACHE_TAGTYPE_VAR &&
               current_tag.type != MUSTACHE_TAGTYPE_VERBATIMVAR &&
               current_tag.type != MUSTACHE_TAGTYPE_VERBATIMVAR2 &&
               (current_tag.end >= templ_size || MUSTACHE_ISNEWLINE(templ_data[current_tag.end])))
            {
                off_t tmp_off = current_tag.beg;
                while(tmp_off > 0 && MUSTACHE_ISWHITESPACE(templ_data[tmp_off-1]))
                    tmp_off--;
                if(tmp_off == 0 || MUSTACHE_ISNEWLINE(templ_data[tmp_off-1])) {
                    current_tag.beg = tmp_off;

                    if(current_tag.end < templ_size && templ_data[current_tag.end] == '\r')
                        current_tag.end++;
                    if(current_tag.end < templ_size && templ_data[current_tag.end] == '\n')
                        current_tag.end++;
                }
            }

            while(current_tag.name_end > current_tag.name_beg  &&
                        MUSTACHE_ISWHITESPACE(templ_data[current_tag.name_end-1]))
                current_tag.name_end--;

            if(current_tag.type != MUSTACHE_TAGTYPE_COMMENT) {
                if(current_tag.name_end <= current_tag.name_beg) {
                    parser->parse_error(MUSTACHE_ERR_NOTAGNAME,
                            mustache_err_messages[MUSTACHE_ERR_NOTAGNAME],
                            (unsigned)current_tag.line, (unsigned)current_tag.col,
                            parser_data);
                    n_errors++;
                }
            }

            /* Remember the tag info. */
            if(n_tags >= n_alloced_tags) {
                unsigned new_n_alloced_tags = (n_alloced_tags > 0 ? n_alloced_tags * 2 : 8);
                MUSTACHE_TAGINFO* new_tags;

                new_tags = (MUSTACHE_TAGINFO*) realloc(tags,
                                new_n_alloced_tags * sizeof(MUSTACHE_TAGINFO));
                if(new_tags == NULL) {
                    parser->sys_error(ENOMEM, parser_data);
                    goto sys_err;
                }

                n_alloced_tags = new_n_alloced_tags;
                tags = new_tags;
            }
            memcpy(&tags[n_tags], &current_tag, sizeof(MUSTACHE_TAGINFO));
            current_tag.type = MUSTACHE_TAGTYPE_NONE;
            n_tags++;
        } else if(MUSTACHE_ISNEWLINE(templ_data[off])) {
            /* Handle end of line. */

            if(current_tag.type != MUSTACHE_TAGTYPE_NONE  &&  current_tag.type != MUSTACHE_TAGTYPE_COMMENT) {
                parser->parse_error(MUSTACHE_ERR_DANGLINGTAGOPENER,
                        mustache_err_messages[MUSTACHE_ERR_DANGLINGTAGOPENER],
                        (unsigned)current_tag.line, (unsigned)current_tag.col,
                        parser_data);
                n_errors++;
                current_tag.type = MUSTACHE_TAGTYPE_NONE;
            }

            /* New line may be formed by digraph "\r\n". */
            if(templ_data[off] == '\r')
                off++;
            if(off < templ_size  &&  templ_data[off] == '\n')
                off++;

            line++;
            col = 1;
        } else {
            /* Handle any other character. */
            off++;
            col++;
        }
    }

    /* Add an extra dummy tag marking end of the template. */
    if(n_tags >= n_alloced_tags) {
        unsigned new_n_alloced_tags = n_alloced_tags + 1;
        MUSTACHE_TAGINFO* new_tags;

        new_tags = (MUSTACHE_TAGINFO*) realloc(tags,
                        new_n_alloced_tags * sizeof(MUSTACHE_TAGINFO));
        if(new_tags == NULL) {
            parser->sys_error(ENOMEM, parser_data);
            goto sys_err;
        }

        n_alloced_tags = new_n_alloced_tags;
        tags = new_tags;
    }
    tags[n_tags].type = MUSTACHE_TAGTYPE_NONE;
    tags[n_tags].beg = templ_size;
    n_tags++;

    /* Success? */
    if(n_errors == 0) {
        *p_tags = tags;
        *p_n_tags = n_tags;
        return 0;
    }

    /* Error path. */
sys_err:
    free(tags);
    *p_tags = NULL;
    *p_n_tags = 0;
    return -1;
}


typedef struct MUSTACHE_BUILD {
    uint8_t* insns;
    size_t n;
    size_t alloc;
} MUSTACHE_BUILD;

static int
mustache_build_append(MUSTACHE_BUILD* build,
                      const MUSTACHE_PARSER* parser, void* parser_data,
                      const uint8_t* data, size_t n)
{
    if(build->n + n > build->alloc) {
        size_t new_alloc = (build->n + n) * 2;
        uint8_t* new_insns;

        new_insns = (uint8_t*) realloc(build->insns, new_alloc);
        if(new_insns == NULL) {
            parser->sys_error(ENOMEM, parser_data);
            return -1;
        }

        build->insns = new_insns;
        build->alloc = new_alloc;
    }

    memcpy(build->insns + build->n, data, n);
    build->n += n;
    return 0;
}

static int
mustache_build_append_num(MUSTACHE_BUILD* build,
                          const MUSTACHE_PARSER* parser, void* parser_data,
                          uint64_t num)
{
    uint8_t tmp[16];
    size_t n = 0;

    while(num >= 0x80) {
        tmp[15 - n++] = 0x80 | (num & 0x7f);
        num = num >> 7;
    }
    tmp[15 - n++] = num;

    return mustache_build_append(build, parser, parser_data, tmp+16-n, n);
}

static uint64_t
mustache_decode_num(const uint8_t* data, off_t off, off_t* p_off)
{
    uint64_t num = 0;

    while(data[off] >= 0x80) {
        num = num << 7;
        num |= (data[off++] & 0x7f);
    }

    num |= data[off++];

    *p_off = off;
    return num;
}

#define MUSTACHE_OP_EXIT            0
#define MUSTACHE_OP_LITERAL         1
#define MUSTACHE_OP_GETNAMED        2
#define MUSTACHE_OP_OUTVERBATIM     3
#define MUSTACHE_OP_OUTESCAPED      4


MUSTACHE_TEMPLATE*
mustache_compile(const char* templ_data, size_t templ_size,
                 const MUSTACHE_PARSER* parser, void* parser_data,
                 unsigned flags)
{
    static const MUSTACHE_PARSER default_parser = { mustache_sys_error, mustache_parse_error };
    MUSTACHE_TAGINFO* tags = NULL;
    unsigned n_tags;
    MUSTACHE_BUILD build = { 0 };
    off_t off;
    MUSTACHE_TAGINFO* tag;

    if(parser == NULL)
        parser = &default_parser;

    /* Collect all tags from the template. */
    if(mustache_parse(templ_data, templ_size, parser, parser_data, &tags, &n_tags) != 0)
        goto err;

    // TODO: Check correctness (compatibility of respective section openings and closings.)

    /* Build the template */
#define APPEND(data, n)                                                       \
        do {                                                                  \
            if(mustache_build_append(&build, parser, parser_data,             \
                                     (const uint8_t*)(data), (n)) != 0)       \
                goto err;                                                     \
        } while(0)

#define APPEND_NUM(num)                                                       \
        do {                                                                  \
            if(mustache_build_append_num(&build, parser, parser_data,         \
                                         (uint64_t)(num)) != 0)               \
                goto err;                                                     \
        } while(0)

    off = 0;
    tag = &tags[0];
    while(1) {
        if(off < tag->beg) {
            /* Handle literal text before the next tag. */
            APPEND_NUM(MUSTACHE_OP_LITERAL);
            APPEND_NUM(tag->beg - off);
            APPEND(templ_data + off, tag->beg - off);
            off = tag->beg;
        }

        /* Handle the end-of-template. */
        if(tag->type == MUSTACHE_TAGTYPE_NONE)
            break;

        switch(tag->type) {
        case MUSTACHE_TAGTYPE_VAR:
        case MUSTACHE_TAGTYPE_VERBATIMVAR:
        case MUSTACHE_TAGTYPE_VERBATIMVAR2:
            APPEND_NUM(MUSTACHE_OP_GETNAMED);
            APPEND_NUM(tag->name_end - tag->name_beg);
            APPEND(templ_data + tag->name_beg, tag->name_end - tag->name_beg);
            APPEND_NUM((tag->type == MUSTACHE_TAGTYPE_VAR) ?
                        MUSTACHE_OP_OUTESCAPED : MUSTACHE_OP_OUTVERBATIM);
            break;

        default:
            break;
        }


        /* Handle var tags. */
        // TODO: MUSTACHE_TAGTYPE_VAR
        // TODO: MUSTACHE_TAGTYPE_VERBATIMVAR + MUSTACHE_TAGTYPE_VERBATIMVAR2

        /* Handle section tags. */
        // TODO: MUSTACHE_TAGTYPE_OPENSECTION
        // TODO: MUSTACHE_TAGTYPE_OPENSECTIONINV
        // TODO: MUSTACHE_TAGTYPE_CLOSESECTION

        /* Handle partials. */
        // TODO: MUSTACHE_TAGTYPE_PARTIAL

        off = tag->end;
        tag++;
    }

    APPEND_NUM(MUSTACHE_OP_EXIT);

    /* Success. */
    free(tags);
    return (MUSTACHE_TEMPLATE*) build.insns;

    /* Error path. */
err:
    free(tags);
    free(build.insns);
    return NULL;
}

void
mustache_release(MUSTACHE_TEMPLATE* t)
{
    if(t == NULL)
        return;

    free(t);
}

int
mustache_process(const MUSTACHE_TEMPLATE* t,
                 const MUSTACHE_RENDERER* renderer, void* renderer_data,
                 const MUSTACHE_DATAPROVIDER* provider, void* provider_data)
{
    const uint8_t* insns = (const uint8_t*) t;
    off_t off = 0;
    void* reg_node = NULL;
    void* current_node = NULL;

    current_node = provider->get_root(provider_data);

    while(1) {
        unsigned opcode = mustache_decode_num(insns, off, &off);

        if(opcode == MUSTACHE_OP_EXIT)
            break;

        switch(opcode) {
        case MUSTACHE_OP_LITERAL:
            {
                size_t n = mustache_decode_num(insns, off, &off);
                if(renderer->out_verbatim((const char*)(insns + off), n, renderer_data) != 0)
                    return -1;
                off += n;
                break;
            }

        case MUSTACHE_OP_GETNAMED:
            {
                size_t n = mustache_decode_num(insns, off, &off);
                reg_node = provider->get_named(current_node, (const char*)(insns + off), n, provider_data);
                off += n;
                break;
            }

        case MUSTACHE_OP_OUTVERBATIM:
        case MUSTACHE_OP_OUTESCAPED:
            if(reg_node != NULL) {
                int (*out)(const char*, size_t, void*);

                out = (opcode == MUSTACHE_OP_OUTVERBATIM) ?
                            renderer->out_verbatim : renderer->out_escaped;
                if(provider->dump(reg_node, out, renderer_data, provider_data) != 0)
                    return -1;
            }
            break;
        }
    }

    return 0;
}
