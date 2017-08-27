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
    return mustache_buffer_insert(buf, buf->n, data, n);
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
    "Tag has no name."
};

/* For the given template, we construct list of MUSTACHE_TAGINFO structures.
 * Along the way, we also check for any parsing errors and report them
 * to the app.
 */

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
    MUSTACHE_BUFFER tags = { 0 };

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

            line++;
            col = 1;
        } else {
            /* Handle any other character. */
            off++;
            col++;
        }
    }

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
 *   Arg #1: Length of the tag name (NUM).
 *   Arg #2: The tag name (STR).
 *
 *   Registers: reg_node is set to the resolved node, or NULL.
 */
#define MUSTACHE_OP_RESOLVE         2

/* Instruction to resolve a tag name.
 *
 *   Arg #1: (Relative) setjmp value (NUM).
 *   Arg #2: Length of the tag name (NUM).
 *   Arg #3: The tag name (STR).
 *
 *   Registers: reg_node is set to the resolved node, or NULL.
 *              reg_failaddr is set to address where to jump.
 */
#define MUSTACHE_OP_RESOLVE_setjmp  3

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
 *            Otherwise, program counter is changed to address in reg_failaddr.
 */
#define MUSTACHE_OP_ENTER           6

/* Instruction to leave a node. The top node in the lookup context stack is
 * popped out.
 */
#define MUSTACHE_OP_LEAVE           7


MUSTACHE_TEMPLATE*
mustache_compile(const char* templ_data, size_t templ_size,
                 const MUSTACHE_PARSER* parser, void* parser_data,
                 unsigned flags)
{
    static const MUSTACHE_PARSER default_parser = { mustache_parse_error };
    MUSTACHE_TAGINFO* tags = NULL;
    unsigned n_tags;
    off_t off;
    MUSTACHE_TAGINFO* tag;
    MUSTACHE_BUFFER insns = { 0 };
    MUSTACHE_STACK jmp_pos_stack = { 0 };
    int done = 0;
    int success = 0;

    if(parser == NULL)
        parser = &default_parser;

    /* Collect all tags from the template. */
    if(mustache_parse(templ_data, templ_size, parser, parser_data, &tags, &n_tags) != 0)
        goto err;

    // TODO: Check correctness (compatibility of respective section openings and closings.)

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
        /* Handle var tags. */
        case MUSTACHE_TAGTYPE_VAR:
        case MUSTACHE_TAGTYPE_VERBATIMVAR:
        case MUSTACHE_TAGTYPE_VERBATIMVAR2:
            APPEND_NUM(MUSTACHE_OP_RESOLVE);
            APPEND_NUM(tag->name_end - tag->name_beg);
            APPEND(templ_data + tag->name_beg, tag->name_end - tag->name_beg);
            APPEND_NUM((tag->type == MUSTACHE_TAGTYPE_VAR) ?
                        MUSTACHE_OP_OUTESCAPED : MUSTACHE_OP_OUTVERBATIM);
            break;

        /* Handle section tags. */
        case MUSTACHE_TAGTYPE_OPENSECTION:
            APPEND_NUM(MUSTACHE_OP_RESOLVE_setjmp);
            PUSH_JMP_POS();
            APPEND_NUM(tag->name_end - tag->name_beg);
            APPEND(templ_data + tag->name_beg, tag->name_end - tag->name_beg);
            APPEND_NUM(MUSTACHE_OP_ENTER);
            break;
        case MUSTACHE_TAGTYPE_CLOSESECTION:
        {
            off_t jmp_pos;

            APPEND_NUM(MUSTACHE_OP_LEAVE);

            /* Set jmp in MUSTACHE_OP_ENTER. */
            jmp_pos = POP_JMP_POS();
            INSERT_NUM(jmp_pos, insns.n - jmp_pos);
            break;
        }

        // TODO: MUSTACHE_TAGTYPE_OPENSECTIONINV

        /* Handle partials. */
        // TODO: MUSTACHE_TAGTYPE_PARTIAL

        /* Handle the end-of-template. */
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
    off_t off = 0;
    off_t reg_failaddr;
    void* reg_node = NULL;
    int done = 0;
    MUSTACHE_STACK node_stack = { 0 };

#define PUSH_NODE()                                                         \
        do {                                                                \
            if(mustache_stack_push(&node_stack, (uintptr_t) reg_node) != 0) \
                goto err;                                                   \
        } while(0)

#define POP_NODE()          mustache_stack_pop(&node_stack)

    reg_node = provider->get_root(provider_data);
    PUSH_NODE();

    while(!done) {
        unsigned opcode = (unsigned) mustache_decode_num(insns, off, &off);

        switch(opcode) {
        case MUSTACHE_OP_LITERAL:
        {
            size_t n = (size_t) mustache_decode_num(insns, off, &off);
            if(renderer->out_verbatim((const char*)(insns + off), n, renderer_data) != 0)
                return -1;
            off += n;
            break;
        }

        case MUSTACHE_OP_RESOLVE_setjmp:
        {
            size_t jmp_len = mustache_decode_num(insns, off, &off);
            reg_failaddr = off + jmp_len;
            /* Pass through */
        }

        case MUSTACHE_OP_RESOLVE:
        {
            void** nodes = (void**) node_stack.data;
            size_t n = node_stack.n / sizeof(void*);
            size_t name_len = (size_t) mustache_decode_num(insns, off, &off);
            const char* name = (const char*)(insns + off);
            off += name_len;

            while(n-- > 0) {
                reg_node = provider->get_named(nodes[n], name, name_len, provider_data);
                if(reg_node != NULL)
                    break;
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
                    return -1;
            }
            break;

        case MUSTACHE_OP_ENTER:
            if(reg_node != NULL)
                PUSH_NODE();
            else
                off = reg_failaddr;
            break;

        case MUSTACHE_OP_LEAVE:
            POP_NODE();
            break;

        case MUSTACHE_OP_EXIT:
            done = 1;
            break;
        }
    }

    return 0;

err:
    return -1;
}
