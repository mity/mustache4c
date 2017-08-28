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

#ifndef MUSTACHE4C_H
#define MUSTACHE4C_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif


#define MUSTACHE_ERR_SUCCESS                (0)
#define MUSTACHE_ERR_DANGLINGTAGOPENER      (1)
#define MUSTACHE_ERR_DANGLINGTAGCLOSER      (2)
#define MUSTACHE_ERR_INCOMPATIBLETAGCLOSER  (3)
#define MUSTACHE_ERR_NOTAGNAME              (4)
#define MUSTACHE_ERR_DANGLINGSECTIONOPENER  (5)
#define MUSTACHE_ERR_DANGLINGSECTIONCLOSER  (6)
#define MUSTACHE_ERR_SECTIONNAMEMISMATCH    (7)
#define MUSTACHE_ERR_SECTIONOPENERHERE      (8)


typedef struct MUSTACHE_PARSER {
    void (*parse_error)(int /*err_code*/, const char* /*msg*/,
                        unsigned /*line*/, unsigned /*column*/, void* /*parser_data*/);
} MUSTACHE_PARSER;


/**
 * An interface the application has to implement, in order to output the result
 * of template processing.
 */
typedef struct MUSTACHE_RENDERER {
    /**
     * Called to output the given text as it is.
     *
     * Non-zero return value aborts mustache_process().
     */
    int (*out_verbatim)(const char* /*output*/, size_t /*size*/, void* /*renderer_data*/);

    /**
     * Called to output the given text. Implementation has to escape it
     * appropriately with respect to the output format. E.g. for HTML output,
     * "<" should be translated to "&lt;" etc.
     *
     * Non-zero return value aborts mustache_process().
     *
     * If no escaping is desired, it can be pointer to the same function
     * as out_verbatim.
     */
    int (*out_escaped)(const char* /*output*/, size_t /*size*/, void* /*renderer_data*/);
} MUSTACHE_RENDERER;


/**
 * An interface the application has to implement, in order to feed
 * mustache_process() with data the template asks for.
 *
 * Tree hierarchy, immutable during the mustache_process() call, is assumed.
 * Each node of the hierarchy has to be uniquely identified by some pointer.
 *
 * The mustache_process() never dereferences any of the pointers. It only
 * uses them to refer to that node when calling any data provider callback.
 */
typedef struct MUSTACHE_DATAPROVIDER {
    /**
     * Called once at the start of mustache_process(). It sets the initial
     * lookup context. */
    void* (*get_root)(void* /*provider_data*/);

    /**
     * Called to get named item of the current node, or NULL if there is no item.
     *
     * If the node is not of appropriate type (e.g. if it is an array of
     * values), NULL has to be returned.
     */
    void* (*get_named)(void* /*node*/, const char* /*name*/, size_t /*size*/, void* /*provider_data*/);

    // TODO: iteration
    //void* (*get_next)(void* /*node*/, void* /*child_node*/, void* /*provider_data*/);
    // OR get_indexed()???
    // OR combination of both???

    /**
     * Called to output contents of the given node. One of the MUSTACHE_PARSER
     * output functions is provided, depending on the type of the mustache tag
     * ( {{...}} versus {{{...}}} ). Implementation of dump() may call that
     * function arbitrarily.
     *
     * In many applications, it is not desirable/expected to be able dumping
     * specific nodes (e.g. if the node is list or array forming the data
     * tree hierarchy). In such cases, the implementation is allowed to just
     * return zero without calling the provided callback at all, output some
     * dummy string (e.g. "<<object>>"), or return non-zero value as an error
     * sign, depending what makes better sense for the application.
     *
     * Implementation of dump() must propagate renderer_data into the
     * callback as its last argument.
     *
     * Non-zero return value aborts mustache_process().
     */
    int (*dump)(void* /*node*/, int (* /*out_fn*/)(const char*, size_t, void*),
                void* /*renderer_data*/, void* /*provider_data*/);
} MUSTACHE_DATAPROVIDER;


typedef struct MUSTACHE_TEMPLATE MUSTACHE_TEMPLATE;


/**
 * Compile template text into a form suitable for mustache_process().
 *
 * If application processes multiple input data with a single template, it is
 * recommended to cache and reuse the compiled template as much as possible,
 * as the compiling may be relatively time-consuming operation.
 *
 * @param templ_data Text of the template.
 * @param templ_size Length of the template text.
 * @param parser Pointer to structure with parser callbacks. May be @c NULL.
 * @param parser_data Pointer just propagated into the parser callbacks.
 * @param allocator Pointer to structure with custom allocator callbacks. May be @c NULL.
 * @param flags Unused, use zero.
 * @return Pointer to the compiled template, or @c NULL on an error.
 */
MUSTACHE_TEMPLATE* mustache_compile(const char* templ_data, size_t templ_size,
                                    const MUSTACHE_PARSER* parser, void* parser_data,
                                    unsigned flags);

/**
 * Release the templated compiled with @c mustache_compile().
 *
 * @param t The template.
 */
void mustache_release(MUSTACHE_TEMPLATE* t);

/**
 * Process the template.
 *
 * The function outputs (via MUSTACHE_RENDERER::out_verbatim()) most of the
 * text of the template. Whenever it reaches a mustache tag, it calls
 * appropriate callback of MUSTACHE_DATAPROVIDER to change lookup context
 * or to output contents of the current context.
 *
 * @param t The template.
 * @param renderer Pointer to structure with output callbacks.
 * @param render_data Pointer just propagated to the output callbacks.
 * @param provider Pointer to structure with data-providing callbacks.
 * @param provider_dara Pointer just propagated to the data-providing callbacks.
 * @return Zero on success, non-zero on failure.
 *
 * Note this operation can fail only if any callback returns an error
 * and aborts the operation.
 */
int mustache_process(const MUSTACHE_TEMPLATE* t,
                     const MUSTACHE_RENDERER* renderer, void* renderer_data,
                     const MUSTACHE_DATAPROVIDER* provider, void* provider_data);


#ifdef __cplusplus
}
#endif

#endif  /* MUSTACHE4C_H */
