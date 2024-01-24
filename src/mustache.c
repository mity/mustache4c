/*
 * Mustache4C
 * (http://github.com/mity/mustache4c)
 *
 * Copyright (c) 2017 Martin Mitáš
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
#include <sys/types.h>  /* for off_t */


#ifdef _MSC_VER
    /* MSVC does not understand "inline" when building as pure C (not C++).
     * However it understands "__inline" */
    #ifndef __cplusplus
        #define inline __inline
    #endif
#endif


#define MUSTACHE_DEFAULTOPENER      "{{"
#define MUSTACHE_DEFAULTCLOSER      "}}"
#define MUSTACHE_MAXOPENERLENGTH    32
#define MUSTACHE_MAXCLOSERLENGTH    32


/**********************
 *** Growing Buffer ***
 **********************/

typedef struct MUSTACHE_BUFFER {
    uint8_t* data;
    size_t n;
    size_t alloc;
} MUSTACHE_BUFFER;

static inline void
mustache_buffer_free(MUSTACHE_BUFFER* buf)
{
    free(buf->data);
}

static int
mustache_buffer_insert(MUSTACHE_BUFFER* buf, off_t off, const void* data, size_t n)
{
    if(buf->n + n > buf->alloc) {
        size_t new_alloc = (buf->n + n) * 2;
        uint8_t* new_data;

        new_data = (uint8_t*) realloc(buf->data, new_alloc);
        if(new_data == NULL)
            return -1;

        buf->data = new_data;
        buf->alloc = new_alloc;
    }

    if(off < buf->n)
        memmove(buf->data + off + n, buf->data + off, buf->n - off);

    memcpy(buf->data + off, data, n);
    buf->n += n;
    return 0;
}

static inline int
mustache_buffer_append(MUSTACHE_BUFFER* buf, const void* data, size_t n)
{
    return mustache_buffer_insert(buf, (off_t) buf->n, data, n);
}

static int
mustache_buffer_insert_num(MUSTACHE_BUFFER* buf, off_t off, uint64_t num)
{
    uint8_t tmp[16];
    size_t n = 0;

    tmp[15 - n++] = num & 0x7f;

    while(1) {
        num = num >> 7;
        if(num == 0)
            break;
        tmp[15 - n++] = 0x80 | (num & 0x7f);
    }

    return mustache_buffer_insert(buf, off, tmp+16-n, n);
}

static inline int
mustache_buffer_append_num(MUSTACHE_BUFFER* buf, uint64_t num)
{
    return mustache_buffer_insert_num(buf, buf->n, num);
}

static uint64_t
mustache_decode_num(const uint8_t* data, off_t off, off_t* p_off)
{
    uint64_t num = 0;

    while(data[off] >= 0x80) {
        num |= (data[off++] & 0x7f);
        num = num << 7;
    }

    num |= data[off++];
    *p_off = off;
    return num;
}


/****************************
 *** Stack Implementation ***
 ****************************/

typedef MUSTACHE_BUFFER MUSTACHE_STACK;

static inline void
mustache_stack_free(MUSTACHE_STACK* stack)
{
    mustache_buffer_free(stack);
}

static inline int
mustache_stack_is_empty(MUSTACHE_STACK* stack)
{
    return (stack->n == 0);
}

static inline int
mustache_stack_push(MUSTACHE_STACK* stack, uintptr_t item)
{
    return mustache_buffer_append(stack, &item, sizeof(uintptr_t));
}

static inline uintptr_t
mustache_stack_peek(MUSTACHE_STACK* stack)
{
    return *((uintptr_t*)(stack->data + (stack->n - sizeof(uintptr_t))));
}

static inline uintptr_t
mustache_stack_pop(MUSTACHE_STACK* stack)
{
    uintptr_t item = mustache_stack_peek(stack);
    stack->n -= sizeof(uintptr_t);
    return item;
}


/***************************
 *** Parsing & Compiling ***
 ***************************/

#define MUSTACHE_ISANYOF2(ch, ch1, ch2)            ((ch) == (ch1) || (ch) == (ch2))
#define MUSTACHE_ISANYOF4(ch, ch1, ch2, ch3, ch4)  ((ch) == (ch1) || (ch) == (ch2) || (ch) == (ch3) || (ch) == (ch4))

#define MUSTACHE_ISWHITESPACE(ch)   MUSTACHE_ISANYOF4((ch), ' ', '\t', '\v', '\f')
#define MUSTACHE_ISNEWLINE(ch)      MUSTACHE_ISANYOF2((ch), '\r', '\n')

/* Keep in sync with MUSTACHE_ERR_xxx constants. */
static const char* mustache_err_messages[] = {
    "Success.",
    "Tag opener has no closer.",
    "Tag closer has no opener.",
    "Tag closer is incompatible with its opener.",
    "Tag has no name.",
    "Tag name is invalid.",
    "Section-opening tag has no closer.",
    "Section-closing tag has no opener.",
    "Name of section-closing tag does not match corresponding section-opening tag.",
    "The section-opening is located here.",
    "Invalid specification of delimiters."
};

/* For the given template, we construct list of MUSTACHE_TAGINFO structures.
 * Along the way, we also check for any parsing errors and report them
 * to the app.
 */

typedef enum MUSTACHE_TAGTYPE {
    MUSTACHE_TAGTYPE_NONE = 0,
    MUSTACHE_TAGTYPE_DELIM,             /* {{=@ @=}} */
    MUSTACHE_TAGTYPE_COMMENT,           /* {{! comment }} */
    MUSTACHE_TAGTYPE_VAR,               /* {{ var }} */
    MUSTACHE_TAGTYPE_VERBATIMVAR,       /* {{{ var }}} */
    MUSTACHE_TAGTYPE_VERBATIMVAR2,      /* {{& var }} */
    MUSTACHE_TAGTYPE_OPENSECTION,       /* {{# section }} */
    MUSTACHE_TAGTYPE_OPENSECTIONINV,    /* {{^ section }} */
    MUSTACHE_TAGTYPE_CLOSESECTION,      /* {{/ section }} */
    MUSTACHE_TAGTYPE_CLOSESECTIONINV,
    MUSTACHE_TAGTYPE_PARTIAL,           /* {{> partial }} */
    MUSTACHE_TAGTYPE_INDENT      /* for internal purposes. */
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
mustache_validate_tagname(const char* tagname, size_t size)
{
    off_t off;

    if(size == 1  &&  tagname[0] == '.')
        return 0;

    /* Verify there is no whitespace and that '.' is used only as a delimiter
     * of non-empty tokens. */
    if(tagname[0] == '.'  ||  tagname[size-1] == '.')
        return -1;
    for(off = 0; off < size; off++) {
        if(MUSTACHE_ISWHITESPACE(tagname[off]))
            return -1;
        if(tagname[off] == '.'  &&  off+1 < size  &&  tagname[off+1] == '.')
            return -1;
    }

    return 0;
}

static int
mustache_validate_sections(const char* templ_data, MUSTACHE_BUFFER* tags_buffer,
                           const MUSTACHE_PARSER* parser, void* parser_data)
{
    MUSTACHE_TAGINFO* tags = (MUSTACHE_TAGINFO*) tags_buffer->data;
    unsigned n_tags = tags_buffer->n / sizeof(MUSTACHE_TAGINFO);
    unsigned i;
    MUSTACHE_STACK section_stack = { 0 };
    MUSTACHE_TAGINFO* opener;
    int n_errors = 0;
    int ret = -1;

    for(i = 0; i < n_tags; i++) {
        switch(tags[i].type) {
        case MUSTACHE_TAGTYPE_OPENSECTION:
        case MUSTACHE_TAGTYPE_OPENSECTIONINV:
            if(mustache_stack_push(&section_stack, (uintptr_t) &tags[i]) != 0)
                goto err;
            break;

        case MUSTACHE_TAGTYPE_CLOSESECTION:
        case MUSTACHE_TAGTYPE_CLOSESECTIONINV:
            if(mustache_stack_is_empty(&section_stack)) {
                parser->parse_error(MUSTACHE_ERR_DANGLINGSECTIONCLOSER,
                        mustache_err_messages[MUSTACHE_ERR_DANGLINGSECTIONCLOSER],
                        (unsigned)tags[i].line, (unsigned)tags[i].col,
                        parser_data);
                n_errors++;
            } else {
                opener = (MUSTACHE_TAGINFO*) mustache_stack_pop(&section_stack);

                if(opener->name_end - opener->name_beg != tags[i].name_end - tags[i].name_beg  ||
                   strncmp(templ_data + opener->name_beg,
                           templ_data + tags[i].name_beg,
                           opener->name_end - opener->name_beg) != 0)
                {
                    parser->parse_error(MUSTACHE_ERR_SECTIONNAMEMISMATCH,
                            mustache_err_messages[MUSTACHE_ERR_SECTIONNAMEMISMATCH],
                            (unsigned)tags[i].line, (unsigned)tags[i].col,
                            parser_data);
                    parser->parse_error(MUSTACHE_ERR_SECTIONOPENERHERE,
                            mustache_err_messages[MUSTACHE_ERR_SECTIONOPENERHERE],
                            (unsigned)opener->line, (unsigned)opener->col,
                            parser_data);
                    n_errors++;
                }

                if(opener->type == MUSTACHE_TAGTYPE_OPENSECTIONINV)
                    tags[i].type = MUSTACHE_TAGTYPE_CLOSESECTIONINV;
            }
            break;

        default:
            break;
        }
    }

    if(!mustache_stack_is_empty(&section_stack)) {
        while(!mustache_stack_is_empty(&section_stack)) {
            opener = (MUSTACHE_TAGINFO*) mustache_stack_pop(&section_stack);

            parser->parse_error(MUSTACHE_ERR_DANGLINGSECTIONOPENER,
                    mustache_err_messages[MUSTACHE_ERR_DANGLINGSECTIONOPENER],
                    (unsigned)opener->line, (unsigned)opener->col,
                    parser_data);
            n_errors++;
        }
    }

    if(n_errors == 0)
        ret = 0;

err:
    mustache_stack_free(&section_stack);
    return ret;
}

static int
mustache_parse_delimiters(const char* delim_spec, size_t size,
                          char* opener, size_t* p_opener_len,
                          char* closer, size_t* p_closer_len)
{
    off_t opener_beg, opener_end;
    off_t closer_beg, closer_end;

    opener_beg = 0;

    opener_end = opener_beg;
    while(opener_end < size) {
        if(MUSTACHE_ISWHITESPACE(delim_spec[opener_end]))
            break;
        if(delim_spec[opener_end] == '=')
            return -1;
        opener_end++;
    }
    if(opener_end <= opener_beg  ||  opener_end - opener_beg > MUSTACHE_MAXOPENERLENGTH)
        return -1;

    closer_beg = opener_end;
    while(closer_beg < size) {
        if(!MUSTACHE_ISWHITESPACE(delim_spec[closer_beg]))
            break;
        closer_beg++;
    }
    if(closer_beg <= opener_end)
        return -1;

    closer_end = closer_beg;
    while(closer_end < size) {
        if(MUSTACHE_ISWHITESPACE(delim_spec[closer_end]))
            return -1;
        closer_end++;
    }
    if(closer_end <= closer_beg  ||   closer_end - closer_beg > MUSTACHE_MAXCLOSERLENGTH)
        return -1;
    if(closer_end != size)
        return -1;

    memcpy(opener, delim_spec + opener_beg, opener_end - opener_beg);
    *p_opener_len = opener_end - opener_beg;
    memcpy(closer, delim_spec + closer_beg, closer_end - closer_beg);
    *p_closer_len = closer_end - closer_beg;
    return 0;
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
    MUSTACHE_BUFFER tags = { 0 };

    /* If this template will ever be used as a partial, it may inherit an
     * extra indentation from parent template, so we mark every line beginning
     * with the dummy tag for further processing in mustache_compile(). */
    if(off < templ_size) {
        current_tag.type = MUSTACHE_TAGTYPE_INDENT;
        current_tag.beg = off;
        current_tag.end = off;
        if(mustache_buffer_append(&tags, &current_tag, sizeof(MUSTACHE_TAGINFO)) != 0)
            goto err;
    }

    current_tag.type = MUSTACHE_TAGTYPE_NONE;

    opener_len = strlen(MUSTACHE_DEFAULTOPENER);
    closer_len = strlen(MUSTACHE_DEFAULTCLOSER);

    while(off < templ_size) {
        int is_opener, is_closer;

        is_opener = (off + opener_len <= templ_size  &&  memcmp(templ_data+off, opener, opener_len) == 0);
        is_closer = (off + closer_len <= templ_size  &&  memcmp(templ_data+off, closer, closer_len) == 0);
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
                case '=':   current_tag.type = MUSTACHE_TAGTYPE_DELIM; off++; break;
                case '!':   current_tag.type = MUSTACHE_TAGTYPE_COMMENT; off++; break;
                case '{':   current_tag.type = MUSTACHE_TAGTYPE_VERBATIMVAR; off++; break;
                case '&':   current_tag.type = MUSTACHE_TAGTYPE_VERBATIMVAR2; off++; break;
                case '#':   current_tag.type = MUSTACHE_TAGTYPE_OPENSECTION; off++; break;
                case '^':   current_tag.type = MUSTACHE_TAGTYPE_OPENSECTIONINV; off++; break;
                case '/':   current_tag.type = MUSTACHE_TAGTYPE_CLOSESECTION; off++; break;
                case '>':   current_tag.type = MUSTACHE_TAGTYPE_PARTIAL; off++; break;
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
            } else if(current_tag.type == MUSTACHE_TAGTYPE_DELIM) {
                /* Maybe we are not really the closer. Maybe the directive
                 * does not change the closer so we are the "new closer" in
                 * something like "{{=<something> }}=}}". */
                if(templ_data[current_tag.name_end - 1] != '='  &&
                   off + closer_len < templ_size  &&
                   templ_data[off] == '='  &&
                   memcmp(templ_data + off + 1, closer, closer_len) == 0)
                {
                    current_tag.name_end += closer_len + 1;
                    off += closer_len + 1;
                    col += closer_len + 1;
                }

                if(templ_data[current_tag.name_end - 1] != '=') {
                    parser->parse_error(MUSTACHE_ERR_INCOMPATIBLETAGCLOSER,
                            mustache_err_messages[MUSTACHE_ERR_INCOMPATIBLETAGCLOSER],
                            (unsigned) line, (unsigned) col, parser_data);
                    n_errors++;
                } else if(current_tag.name_end > current_tag.name_beg) {
                    current_tag.name_end--;     /* Consume the closer's '=' */
                }
            }

            current_tag.end = off;

            /* If the tag is standalone, expand it to consume also any
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

            if(current_tag.type == MUSTACHE_TAGTYPE_DELIM) {
                if(mustache_parse_delimiters(templ_data + current_tag.name_beg,
                        current_tag.name_end - current_tag.name_beg,
                        opener, &opener_len, closer, &closer_len) != 0)
                {
                    parser->parse_error(MUSTACHE_ERR_INVALIDDELIMITERS,
                            mustache_err_messages[MUSTACHE_ERR_INVALIDDELIMITERS],
                            (unsigned)current_tag.line, (unsigned)current_tag.col,
                            parser_data);
                    n_errors++;
                }

                /* From now on, ignore this tag. */
                current_tag.type = MUSTACHE_TAGTYPE_COMMENT;
            }

            if(current_tag.type != MUSTACHE_TAGTYPE_COMMENT) {
                if(mustache_validate_tagname(templ_data + current_tag.name_beg,
                            current_tag.name_end - current_tag.name_beg) != 0) {
                    parser->parse_error(MUSTACHE_ERR_INVALIDTAGNAME,
                            mustache_err_messages[MUSTACHE_ERR_INVALIDTAGNAME],
                            (unsigned)current_tag.line, (unsigned)current_tag.col,
                            parser_data);
                    n_errors++;
                }
            }

            /* Remember the tag info. */
            if(mustache_buffer_append(&tags, &current_tag, sizeof(MUSTACHE_TAGINFO)) != 0)
                goto err;

            current_tag.type = MUSTACHE_TAGTYPE_NONE;
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

            if(current_tag.type == MUSTACHE_TAGTYPE_NONE  &&  off < templ_size) {
                current_tag.type = MUSTACHE_TAGTYPE_INDENT;
                current_tag.beg = off;
                current_tag.end = off;
                if(mustache_buffer_append(&tags, &current_tag, sizeof(MUSTACHE_TAGINFO)) != 0)
                    goto err;
                current_tag.type = MUSTACHE_TAGTYPE_NONE;
            }

            line++;
            col = 1;
        } else {
            /* Handle any other character. */
            off++;
            col++;
        }
    }

    if(mustache_validate_sections(templ_data, &tags, parser, parser_data) != 0)
        goto err;

    /* Add an extra dummy tag marking end of the template. */
    current_tag.type = MUSTACHE_TAGTYPE_NONE;
    current_tag.beg = templ_size;
    current_tag.end = templ_size;
    if(mustache_buffer_append(&tags, &current_tag, sizeof(MUSTACHE_TAGINFO)) != 0)
        goto err;

    /* Success? */
    if(n_errors == 0) {
        *p_tags = (MUSTACHE_TAGINFO*) tags.data;
        *p_n_tags = tags.n / sizeof(MUSTACHE_TAGINFO);
        return 0;
    }

    /* Error path. */
err:
    mustache_buffer_free(&tags);
    *p_tags = NULL;
    *p_n_tags = 0;
    return -1;
}


/* The compiled template is a sequence of following instruction types.
 * The instructions have two types of arguments:
 *  -- NUM: a number encoded with mustache_buffer_[append|insert]_num().
 *  -- STR: a string (always preceded with a NUM denoting its length).
 */

/* Instruction denoting end of template.
 */
#define MUSTACHE_OP_EXIT            0

/* Instruction for outputting a literal text.
 *
 *   Arg #1: Length of the literal string (NUM).
 *   Arg #2: The literal string (STR).
 */
#define MUSTACHE_OP_LITERAL         1

/* Instruction to resolve a tag name.
 *
 *   Arg #1: (Relative) setjmp value (NUM).
 *   Arg #2: Count of names (NUM).
 *   Arg #3: Length of the 1st tag name (NUM).
 *   Arg #4: The tag name (STR).
 *   etc. (more names follow, up to the count in arg #2)
 *
 *   Registers: reg_node is set to the resolved node, or NULL.
 *              reg_jmpaddr is set to address where some next instruction may
 *              want to jump on some condition.
 */
#define MUSTACHE_OP_RESOLVE_setjmp  2

/* Instruction to resolve a tag name.
 *
 *   Arg #1: Count of names (NUM).
 *   Arg #2: Length of the tag name (NUM).
 *   Arg #3: The tag name (STR).
 *   etc. (more names follow, up to the count in arg #1)
 *
 *   Registers: reg_node is set to the resolved node, or NULL.
 */
#define MUSTACHE_OP_RESOLVE         3

/* Instructions to output a node.
 *
 * Registers: If it is not NULL, reg_node determines the node to output.
 *            Otherwise, it is noop.
 */
#define MUSTACHE_OP_OUTVERBATIM     4
#define MUSTACHE_OP_OUTESCAPED      5

/* Instruction to enter a node in register reg_node, i.e. to change a lookup
 * context for resolve instructions.
 *
 * Registers: If it is not NULL, reg_node is pushed to the stack.
 *            Otherwise, program counter is changed to address in reg_jmpaddr.
 */
#define MUSTACHE_OP_ENTER           6

/* Instruction to leave a node. The top node in the lookup context stack is
 * popped out.
 *
 * Arg #1: (Relative) setjmp value (NUM) for jumping back for next loop iteration.
 */
#define MUSTACHE_OP_LEAVE           7

/* Instruction to open inverted section.
 * Note there is no MUSTACHE_OP_LEAVEINV instruction as it is noop.
 *
 * Registers: If reg_node is NULL, continues normally.
 *            Otherwise, program counter is changed to address in reg_jmpaddr.
 */
#define MUSTACHE_OP_ENTERINV        8

/* Instruction to enter a partial.
 *
 * Arg #1: Length of the partial name (NUM).
 * Arg #2: The partial name (STR).
 * Arg #3: Length of the indentation string (NUM).
 * Arg #4: Indentation, i.e. string composed of whitespace characters (STR).
 */
#define MUSTACHE_OP_PARTIAL         9

/* Instruction to insert extra indentation (inherited from parent templates).
 */
#define MUSTACHE_OP_INDENT          10


static int
mustache_compile_tagname(MUSTACHE_BUFFER* insns, const char* name, size_t size)
{
    unsigned n_tokens = 1;
    unsigned i;
    off_t tok_beg, tok_end;

    if(size == 1  &&  name[0] == '.') {
        /* Implicit iterator. */
        n_tokens = 0;
    } else {
        for(i = 0; i < size; i++) {
            if(name[i] == '.')
                n_tokens++;
        }
    }

    if(mustache_buffer_append_num(insns, n_tokens) != 0)
        return -1;

    tok_beg = 0;
    for(i = 0; i < n_tokens; i++) {
        tok_end = tok_beg;
        while(tok_end < size  &&  name[tok_end] != '.')
            tok_end++;

        if(mustache_buffer_append_num(insns, tok_end - tok_beg) != 0)
            return -1;
        if(mustache_buffer_append(insns, name + tok_beg, tok_end - tok_beg) != 0)
            return -1;

        tok_beg = tok_end + 1;
    }

    return 0;
}

MUSTACHE_TEMPLATE*
mustache_compile(const char* templ_data, size_t templ_size,
                 const MUSTACHE_PARSER* parser, void* parser_data,
                 unsigned flags)
{
    static const MUSTACHE_PARSER default_parser = { mustache_parse_error };
    MUSTACHE_TAGINFO* tags = NULL;
    unsigned n_tags;
    off_t off;
    off_t jmp_pos;
    MUSTACHE_TAGINFO* tag;
    MUSTACHE_BUFFER insns = { 0 };
    MUSTACHE_STACK jmp_pos_stack = { 0 };
    int done = 0;
    int success = 0;
    size_t indent_len;

    if(parser == NULL)
        parser = &default_parser;

    /* Collect all tags from the template. */
    if(mustache_parse(templ_data, templ_size, parser, parser_data, &tags, &n_tags) != 0)
        goto err;

    /* Build the template */
#define APPEND(data, n)                                                                 \
        do {                                                                            \
            if(mustache_buffer_append(&insns, (data), (n)) != 0)                        \
                goto err;                                                               \
        } while(0)

#define APPEND_NUM(num)                                                                 \
        do {                                                                            \
            if(mustache_buffer_append_num(&insns, (uint64_t)(num)) != 0)                \
                goto err;                                                               \
        } while(0)

#define APPEND_TAGNAME(tag)                                                             \
        do {                                                                            \
            if(mustache_compile_tagname(&insns, templ_data + (tag)->name_beg,           \
                                        (tag)->name_end - (tag)->name_beg) != 0)        \
                goto err;                                                               \
        } while(0)

#define INSERT_NUM(pos, num)                                                            \
        do {                                                                            \
            if(mustache_buffer_insert_num(&insns, (pos), (uint64_t)(num)) != 0)         \
                goto err;                                                               \
        } while(0)

#define PUSH_JMP_POS()                                                                  \
        do {                                                                            \
            if(mustache_stack_push(&jmp_pos_stack, insns.n) != 0)                       \
                goto err;                                                               \
        } while(0)

#define POP_JMP_POS()       ((off_t) mustache_stack_pop(&jmp_pos_stack))

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

        switch(tag->type) {
        case MUSTACHE_TAGTYPE_VAR:
        case MUSTACHE_TAGTYPE_VERBATIMVAR:
        case MUSTACHE_TAGTYPE_VERBATIMVAR2:
            APPEND_NUM(MUSTACHE_OP_RESOLVE);
            APPEND_TAGNAME(tag);
            APPEND_NUM((tag->type == MUSTACHE_TAGTYPE_VAR) ?
                        MUSTACHE_OP_OUTESCAPED : MUSTACHE_OP_OUTVERBATIM);
            break;

        case MUSTACHE_TAGTYPE_OPENSECTION:
            APPEND_NUM(MUSTACHE_OP_RESOLVE_setjmp);
            PUSH_JMP_POS();
            APPEND_TAGNAME(tag);
            APPEND_NUM(MUSTACHE_OP_ENTER);
            PUSH_JMP_POS();
            break;

        case MUSTACHE_TAGTYPE_CLOSESECTION:
            APPEND_NUM(MUSTACHE_OP_LEAVE);
            APPEND_NUM(insns.n - POP_JMP_POS());
            jmp_pos = POP_JMP_POS();
            INSERT_NUM(jmp_pos, insns.n - jmp_pos);
            break;

        case MUSTACHE_TAGTYPE_OPENSECTIONINV:
            APPEND_NUM(MUSTACHE_OP_RESOLVE_setjmp);
            PUSH_JMP_POS();
            APPEND_TAGNAME(tag);
            APPEND_NUM(MUSTACHE_OP_ENTERINV);
            break;

        case MUSTACHE_TAGTYPE_CLOSESECTIONINV:
            jmp_pos = POP_JMP_POS();
            INSERT_NUM(jmp_pos, insns.n - jmp_pos);
            break;

        case MUSTACHE_TAGTYPE_PARTIAL:
            APPEND_NUM(MUSTACHE_OP_PARTIAL);
            APPEND_NUM(tag->name_end - tag->name_beg);
            APPEND(templ_data + tag->name_beg, tag->name_end - tag->name_beg);
            indent_len = 0;
            while(MUSTACHE_ISWHITESPACE(templ_data[tag->beg + indent_len]))
                indent_len++;
            APPEND_NUM(indent_len);
            APPEND(templ_data + tag->beg, indent_len);
            break;

        case MUSTACHE_TAGTYPE_INDENT:
            APPEND_NUM(MUSTACHE_OP_INDENT);
            break;

        case MUSTACHE_TAGTYPE_NONE:
            APPEND_NUM(MUSTACHE_OP_EXIT);
            done = 1;
            break;

        default:
            break;
        }

        if(done)
            break;

        off = tag->end;
        tag++;
    }

    success = 1;

err:
    free(tags);
    mustache_buffer_free(&jmp_pos_stack);
    if(success) {
        return (MUSTACHE_TEMPLATE*) insns.data;
    } else {
        mustache_buffer_free(&insns);
        return NULL;
    }
}

void
mustache_release(MUSTACHE_TEMPLATE* t)
{
    if(t == NULL)
        return;

    free(t);
}


/**********************************
 *** Applying Compiled Template ***
 **********************************/

int
mustache_process(const MUSTACHE_TEMPLATE* t,
                 const MUSTACHE_RENDERER* renderer, void* renderer_data,
                 const MUSTACHE_DATAPROVIDER* provider, void* provider_data)
{
    const uint8_t* insns = (const uint8_t*) t;
    off_t reg_pc = 0;       /* Program counter register. */
    off_t reg_jmpaddr;      /* Jump target address register. */
    void* reg_node = NULL;  /* Working node register. */
    int done = 0;
    MUSTACHE_STACK node_stack = { 0 };
    MUSTACHE_STACK index_stack = { 0 };
    MUSTACHE_STACK partial_stack = { 0 };
    MUSTACHE_BUFFER indent_buffer = { 0 };
    int ret = -1;

#define PUSH_NODE()                                                         \
        do {                                                                \
            if(mustache_stack_push(&node_stack, (uintptr_t) reg_node) != 0) \
                goto err;                                                   \
        } while(0)

#define POP_NODE()          ((void*) mustache_stack_pop(&node_stack))

#define PEEK_NODE()         ((void*) mustache_stack_peek(&node_stack))

#define PUSH_INDEX(index)                                                   \
        do {                                                                \
            if(mustache_stack_push(&index_stack, (uintptr_t) (index)) != 0) \
                goto err;                                                   \
        } while(0)

#define POP_INDEX()         ((unsigned) mustache_stack_pop(&index_stack))

    reg_node = provider->get_root(provider_data);
    PUSH_NODE();

    while(!done) {
        unsigned opcode = (unsigned) mustache_decode_num(insns, reg_pc, &reg_pc);

        switch(opcode) {
        case MUSTACHE_OP_LITERAL:
        {
            size_t n = (size_t) mustache_decode_num(insns, reg_pc, &reg_pc);
            if(renderer->out_verbatim((const char*)(insns + reg_pc), n, renderer_data) != 0)
                goto err;
            reg_pc += n;
            break;
        }

        case MUSTACHE_OP_RESOLVE_setjmp:
        {
            size_t jmp_len = (size_t) mustache_decode_num(insns, reg_pc, &reg_pc);
            reg_jmpaddr = reg_pc + jmp_len;
            /* Pass through */
        }

        case MUSTACHE_OP_RESOLVE:
        {
            unsigned n_names = (unsigned) mustache_decode_num(insns, reg_pc, &reg_pc);
            unsigned i;

            if(n_names == 0) {
                /* Implicit iterator. */
                reg_node = PEEK_NODE();
                break;
            }

            for(i = 0; i < n_names; i++) {
                size_t name_len = (size_t) mustache_decode_num(insns, reg_pc, &reg_pc);
                const char* name = (const char*)(insns + reg_pc);
                reg_pc += name_len;

                if(i == 0) {
                    void** nodes = (void**) node_stack.data;
                    size_t n_nodes = node_stack.n / sizeof(void*);

                    while(n_nodes-- > 0) {
                        reg_node = provider->get_child_by_name(nodes[n_nodes], 
                                        name, name_len, provider_data);
                        if(reg_node != NULL)
                            break;
                    }
                } else if(reg_node != NULL) {
                    reg_node = provider->get_child_by_name(reg_node,
                                        name, name_len, provider_data);
                }
            }
            break;
        }

        case MUSTACHE_OP_OUTVERBATIM:
        case MUSTACHE_OP_OUTESCAPED:
            if(reg_node != NULL) {
                int (*out)(const char*, size_t, void*);

                out = (opcode == MUSTACHE_OP_OUTVERBATIM) ?
                            renderer->out_verbatim : renderer->out_escaped;
                if(provider->dump(reg_node, out, renderer_data, provider_data) != 0)
                    goto err;
            }
            break;

        case MUSTACHE_OP_ENTER:
            if(reg_node != NULL) {
                PUSH_NODE();
                reg_node = provider->get_child_by_index(reg_node, 0, provider_data);
                if(reg_node != NULL) {
                    PUSH_NODE();
                    PUSH_INDEX(0);
                } else {
                    (void) POP_NODE();
                }
            }
            if(reg_node == NULL)
                reg_pc = reg_jmpaddr;
            break;

        case MUSTACHE_OP_LEAVE:
        {
            off_t jmp_base = reg_pc;
            size_t jmp_len = (size_t) mustache_decode_num(insns, reg_pc, &reg_pc);
            unsigned index = POP_INDEX();

            (void) POP_NODE();
            reg_node = provider->get_child_by_index(PEEK_NODE(), ++index, provider_data);
            if(reg_node != NULL) {
                PUSH_NODE();
                PUSH_INDEX(index);
                reg_pc = jmp_base - jmp_len;
            } else {
                (void) POP_NODE();
            }
            break;
        }

        case MUSTACHE_OP_ENTERINV:
            if(reg_node == NULL  ||  provider->get_child_by_index(reg_node,
                                                0, provider_data) == NULL) {
                /* Resolve failed: Noop, continue normally. */
            } else {
                reg_pc = reg_jmpaddr;
            }
            break;

        case MUSTACHE_OP_PARTIAL:
        {
            size_t name_len;
            const char* name;
            size_t indent_len;
            const char* indent;
            MUSTACHE_TEMPLATE* partial;

            name_len = mustache_decode_num(insns, reg_pc, &reg_pc);
            name = (const char*) (insns + reg_pc);
            reg_pc += name_len;

            indent_len = mustache_decode_num(insns, reg_pc, &reg_pc);
            indent = (const char*) (insns + reg_pc);
            reg_pc += indent_len;

            partial = provider->get_partial(name, name_len, provider_data);
            if(partial != NULL) {
                if(mustache_stack_push(&partial_stack, (uintptr_t) insns) != 0)
                    goto err;
                if(mustache_stack_push(&partial_stack, (uintptr_t) reg_pc) != 0)
                    goto err;
                if(mustache_stack_push(&partial_stack, (uintptr_t) indent_len) != 0)
                    goto err;
                if(mustache_buffer_append(&indent_buffer, indent, indent_len) != 0)
                    goto err;
                reg_pc = 0;
                insns = (uint8_t*) partial;
            }
            break;
        }

        case MUSTACHE_OP_INDENT:
            if(renderer->out_verbatim((const char*)(indent_buffer.data),
                                indent_buffer.n, renderer_data) != 0)
                goto err;
            break;

        case MUSTACHE_OP_EXIT:
            if(mustache_stack_is_empty(&partial_stack)) {
                done = 1;
            } else {
                size_t indent_len = (size_t) mustache_stack_pop(&partial_stack);
                reg_pc = (off_t) mustache_stack_pop(&partial_stack);
                insns = (uint8_t*) mustache_stack_pop(&partial_stack);

                indent_buffer.n -= indent_len;
            }
            break;
        }
    }

    /* Success. */
    ret = 0;

err:
    mustache_stack_free(&node_stack);
    mustache_stack_free(&index_stack);
    mustache_stack_free(&partial_stack);
    mustache_buffer_free(&indent_buffer);
    return ret;
}
