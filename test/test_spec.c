
#include "acutest.h"
#include "mustache.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <sys/types.h>  /* for off_t */


typedef struct BUFFER {
    char data[1024];
    size_t n;
} BUFFER;


/****************************************************
 *** Implementation of MUSTACHE_PARSER interface. ***
 ****************************************************/

static void
parse_error(int err_code, const char* msg, unsigned line, unsigned col, void* data)
{
    BUFFER* buf = (BUFFER*) data;
    buf->n += sprintf(buf->data, "Error: %u:%u: %s\n", line, col, msg);
}

static const MUSTACHE_PARSER parser = {
    parse_error
};


/******************************************************
 *** Implementation of MUSTACHE_RENDERER interface. ***
 ******************************************************/

static int
out(const char* output, size_t n, void* data)
{
    BUFFER* buf = (BUFFER*) data;

    memcpy(buf->data + buf->n, output, n);
    buf->n += n;
    return 0;
}

static int
out_escaped(const char* output, size_t n, void* data)
{
    off_t i;

    for(i = 0; i < n; i++) {
        switch(output[i]) {
            case '&':   out("&amp;", 5, data); break;
            case '"':   out("&quot;", 6, data); break;
            case '<':   out("&lt;", 4, data); break;
            case '>':   out("&gt;", 4, data); break;
            default:    out(output + i, 1, data); break;
        }
    }

    return 0;
}

static const MUSTACHE_RENDERER renderer = {
    out,
    out_escaped,
};


/**********************************************************
 *** Implementation of MUSTACHE_DATAPROVIDER interface. ***
 **********************************************************/

static int
dump(void* node, int (*out_fn)(const char*, size_t, void*), void* renderer_data, void* data)
{
    JSON_VALUE* value = (JSON_VALUE*) node;

    switch(value->type) {
    case JSON_NULL:
    case JSON_FALSE:
        /* no output. */
        return 0;

    case JSON_TRUE:
        return out_fn("<<TRUE>>", strlen("<<TRUE>>"), renderer_data);
    case JSON_ARRAY:
        return out_fn("<<ARRAY>>", strlen("<<ARRAY>>"), renderer_data);
    case JSON_OBJECT:
        return out_fn("<<OBJECT>>", strlen("<<OBJECT>>"), renderer_data);

    case JSON_STRING:
        return out_fn(value->data.str, strlen(value->data.str), renderer_data);
    }

    return 0;
}

static void*
get_root(void* data)
{
    /* We pass the JSON root value as the provider data. */
    return data;
}

static void*
get_named(void* node, const char* name, size_t size, void* data)
{
    JSON_VALUE* value = (JSON_VALUE*) node;
    unsigned i;

    if(value->type != JSON_OBJECT)
        return NULL;

    for(i = 0; i < value->data.obj.n; i++) {
        const char* child_key = value->data.obj.keys[i];
        if(strlen(child_key) == size && strncmp(child_key, name, size) == 0) {
            JSON_VALUE* child_value = value->data.obj.values[i];
            if(child_value->type == JSON_NULL || child_value->type == JSON_FALSE)
                return NULL;
            return (void*) child_value;
        }
    }

    return NULL;
}

static void*
get_indexed(void* node, unsigned index, void* data)
{
    JSON_VALUE* value = (JSON_VALUE*) node;

    if(value->type == JSON_NULL || value->type == JSON_FALSE)
        return NULL;

    if(value->type == JSON_ARRAY  &&  index < value->data.array.n)
        return (void*) value->data.array.values[index];
    else if(value->type != JSON_ARRAY  &&  index == 0)
        return (void*) value;

    return NULL;
}

static const MUSTACHE_DATAPROVIDER provider = {
    dump,
    get_root,
    get_named,
    get_indexed
};


/*********************************
 *** Main body for test units. ***
 *********************************/

static void
run(const char* desc, const char* templ, const char* data, const char* partials, const char* expected)
{
    JSON_VALUE* json_root;
    MUSTACHE_TEMPLATE* t;
    BUFFER buf = { 0 };

    json_root = json_parse(data);
    TEST_CHECK(json_root != NULL);

    t = mustache_compile(templ, strlen(templ), &parser, (void*) &buf, 0);
    if(t != NULL)
        mustache_process(t, &renderer, (void*) &buf, &provider, json_root);

    if(!TEST_CHECK_(t != NULL  &&
                    buf.n == strlen(expected)  &&
                    memcmp(expected, buf.data, buf.n) == 0, "%s", desc))
    {
        TEST_MSG("Template:");
        TEST_MSG("---------");
        TEST_MSG("%s", templ);
        TEST_MSG("\nData:");
        TEST_MSG("---------");
        TEST_MSG("%s", data);
        if(partials != NULL) {
            TEST_MSG("\nPartials:");
            TEST_MSG("---------");
            TEST_MSG("%s", partials);
        }
        TEST_MSG("\nExpected:");
        TEST_MSG("---------");
        TEST_MSG("%s", expected);
        TEST_MSG("\nProduced:");
        TEST_MSG("---------");
        TEST_MSG("%.*s", buf.n, buf.data);
    }

    json_free(json_root);
    mustache_release(t);
}


/***********************
 *** The test units. ***
 ***********************/

/* Note all the test functions below have been generated by script/maketests.py
 * from {{musctache}} specification (https://github.com/mustache/spec.
 * Do not modify it manually.
 */

static void
test_comments_1(void)
{
    run(
        "comment blocks should be removed from the template",
        "12345{{! Comment Block! }}67890",
        "{}",
        NULL,
        "1234567890"
    );
}

static void
test_comments_2(void)
{
    run(
        "multiline comments should be permitted",
        "12345{{!\n  This is a\n  multi-line comment...\n}}67890\n",
        "{}",
        NULL,
        "1234567890\n"
    );
}

static void
test_comments_3(void)
{
    run(
        "all standalone comment lines should be removed",
        "Begin.\n{{! Comment Block! }}\nEnd.\n",
        "{}",
        NULL,
        "Begin.\nEnd.\n"
    );
}

static void
test_comments_4(void)
{
    run(
        "all standalone comment lines should be removed",
        "Begin.\n  {{! Indented Comment Block! }}\nEnd.\n",
        "{}",
        NULL,
        "Begin.\nEnd.\n"
    );
}

static void
test_comments_5(void)
{
    run(
        "\"\\r\\n\" should be considered a newline for standalone tags",
        "|\r\n{{! Standalone Comment }}\r\n|",
        "{}",
        NULL,
        "|\r\n|"
    );
}

static void
test_comments_6(void)
{
    run(
        "standalone tags should not require a newline to precede them",
        "  {{! I'm Still Standalone }}\n!",
        "{}",
        NULL,
        "!"
    );
}

static void
test_comments_7(void)
{
    run(
        "standalone tags should not require a newline to follow them",
        "!\n  {{! I'm Still Standalone }}",
        "{}",
        NULL,
        "!\n"
    );
}

static void
test_comments_8(void)
{
    run(
        "all standalone comment lines should be removed",
        "Begin.\n{{!\nSomething's going on here...\n}}\nEnd.\n",
        "{}",
        NULL,
        "Begin.\nEnd.\n"
    );
}

static void
test_comments_9(void)
{
    run(
        "all standalone comment lines should be removed",
        "Begin.\n  {{!\n    Something's going on here...\n  }}\nEnd.\n",
        "{}",
        NULL,
        "Begin.\nEnd.\n"
    );
}

static void
test_comments_10(void)
{
    run(
        "inline comments should not strip whitespace",
        "  12 {{! 34 }}\n",
        "{}",
        NULL,
        "  12 \n"
    );
}

static void
test_comments_11(void)
{
    run(
        "comment removal should preserve surrounding whitespace",
        "12345 {{! Comment Block! }} 67890",
        "{}",
        NULL,
        "12345  67890"
    );
}

static void
test_delimiters_1(void)
{
    run(
        "the equals sign (used on both sides) should permit delimiter changes",
        "{{=<% %>=}}(<%text%>)",
        "{\"text\": \"Hey!\"}",
        NULL,
        "(Hey!)"
    );
}

static void
test_delimiters_2(void)
{
    run(
        "characters with special meaning regexen should be valid delimiters",
        "({{=[ ]=}}[text])",
        "{\"text\": \"It worked!\"}",
        NULL,
        "(It worked!)"
    );
}

static void
test_delimiters_3(void)
{
    run(
        "delimiters set outside sections should persist",
        "[\n{{#section}}\n  {{data}}\n  |data|\n{{/section}}\n\n{{= | | =}}\n|#section|\n  {{data}}\n  |data|\n|/section|\n]\n",
        "{\"section\": true, \"data\": \"I got interpolated.\"}",
        NULL,
        "[\n  I got interpolated.\n  |data|\n\n  {{data}}\n  I got interpolated.\n]\n"
    );
}

static void
test_delimiters_4(void)
{
    run(
        "delimiters set outside inverted sections should persist",
        "[\n{{^section}}\n  {{data}}\n  |data|\n{{/section}}\n\n{{= | | =}}\n|^section|\n  {{data}}\n  |data|\n|/section|\n]\n",
        "{\"section\": false, \"data\": \"I got interpolated.\"}",
        NULL,
        "[\n  I got interpolated.\n  |data|\n\n  {{data}}\n  I got interpolated.\n]\n"
    );
}

static void
test_delimiters_5(void)
{
    run(
        "delimiters set in a parent template should not affect a partial",
        "[ {{>include}} ]\n{{= | | =}}\n[ |>include| ]\n",
        "{\"value\": \"yes\"}",
        "{\"include\": \".{{value}}.\"}",
        "[ .yes. ]\n[ .yes. ]\n"
    );
}

static void
test_delimiters_6(void)
{
    run(
        "delimiters set in a partial should not affect the parent template",
        "[ {{>include}} ]\n[ .{{value}}.  .|value|. ]\n",
        "{\"value\": \"yes\"}",
        "{\"include\": \".{{value}}. {{= | | =}} .|value|.\"}",
        "[ .yes.  .yes. ]\n[ .yes.  .|value|. ]\n"
    );
}

static void
test_delimiters_7(void)
{
    run(
        "surrounding whitespace should be left untouched",
        "| {{=@ @=}} |",
        "{}",
        NULL,
        "|  |"
    );
}

static void
test_delimiters_8(void)
{
    run(
        "whitespace should be left untouched",
        " | {{=@ @=}}\n",
        "{}",
        NULL,
        " | \n"
    );
}

static void
test_delimiters_9(void)
{
    run(
        "standalone lines should be removed from the template",
        "Begin.\n{{=@ @=}}\nEnd.\n",
        "{}",
        NULL,
        "Begin.\nEnd.\n"
    );
}

static void
test_delimiters_10(void)
{
    run(
        "indented standalone lines should be removed from the template",
        "Begin.\n  {{=@ @=}}\nEnd.\n",
        "{}",
        NULL,
        "Begin.\nEnd.\n"
    );
}

static void
test_delimiters_11(void)
{
    run(
        "\"\\r\\n\" should be considered a newline for standalone tags",
        "|\r\n{{= @ @ =}}\r\n|",
        "{}",
        NULL,
        "|\r\n|"
    );
}

static void
test_delimiters_12(void)
{
    run(
        "standalone tags should not require a newline to precede them",
        "  {{=@ @=}}\n=",
        "{}",
        NULL,
        "="
    );
}

static void
test_delimiters_13(void)
{
    run(
        "standalone tags should not require a newline to follow them",
        "=\n  {{=@ @=}}",
        "{}",
        NULL,
        "=\n"
    );
}

static void
test_delimiters_14(void)
{
    run(
        "superfluous in-tag whitespace should be ignored",
        "|{{= @   @ =}}|",
        "{}",
        NULL,
        "||"
    );
}

static void
test_interpolation_1(void)
{
    run(
        "mustache-free templates should render as-is",
        "Hello from {Mustache}!\n",
        "{}",
        NULL,
        "Hello from {Mustache}!\n"
    );
}

static void
test_interpolation_2(void)
{
    run(
        "unadorned tags should interpolate content into the template",
        "Hello, {{subject}}!\n",
        "{\"subject\": \"world\"}",
        NULL,
        "Hello, world!\n"
    );
}

static void
test_interpolation_3(void)
{
    run(
        "basic interpolation should be html escaped",
        "These characters should be HTML escaped: {{forbidden}}\n",
        "{\"forbidden\": \"& \\\" < >\"}",
        NULL,
        "These characters should be HTML escaped: &amp; &quot; &lt; &gt;\n"
    );
}

static void
test_interpolation_4(void)
{
    run(
        "triple mustaches should interpolate without html escaping",
        "These characters should not be HTML escaped: {{{forbidden}}}\n",
        "{\"forbidden\": \"& \\\" < >\"}",
        NULL,
        "These characters should not be HTML escaped: & \" < >\n"
    );
}

static void
test_interpolation_5(void)
{
    run(
        "ampersand should interpolate without html escaping",
        "These characters should not be HTML escaped: {{&forbidden}}\n",
        "{\"forbidden\": \"& \\\" < >\"}",
        NULL,
        "These characters should not be HTML escaped: & \" < >\n"
    );
}

static void
test_interpolation_6(void)
{
    run(
        "integers should interpolate seamlessly",
        "\"{{mph}} miles an hour!\"",
        "{\"mph\": 85}",
        NULL,
        "\"85 miles an hour!\""
    );
}

static void
test_interpolation_7(void)
{
    run(
        "integers should interpolate seamlessly",
        "\"{{{mph}}} miles an hour!\"",
        "{\"mph\": 85}",
        NULL,
        "\"85 miles an hour!\""
    );
}

static void
test_interpolation_8(void)
{
    run(
        "integers should interpolate seamlessly",
        "\"{{&mph}} miles an hour!\"",
        "{\"mph\": 85}",
        NULL,
        "\"85 miles an hour!\""
    );
}

static void
test_interpolation_9(void)
{
    run(
        "decimals should interpolate seamlessly with proper significance",
        "\"{{power}} jiggawatts!\"",
        "{\"power\": 1.21}",
        NULL,
        "\"1.21 jiggawatts!\""
    );
}

static void
test_interpolation_10(void)
{
    run(
        "decimals should interpolate seamlessly with proper significance",
        "\"{{{power}}} jiggawatts!\"",
        "{\"power\": 1.21}",
        NULL,
        "\"1.21 jiggawatts!\""
    );
}

static void
test_interpolation_11(void)
{
    run(
        "decimals should interpolate seamlessly with proper significance",
        "\"{{&power}} jiggawatts!\"",
        "{\"power\": 1.21}",
        NULL,
        "\"1.21 jiggawatts!\""
    );
}

static void
test_interpolation_12(void)
{
    run(
        "failed context lookups should default to empty strings",
        "I ({{cannot}}) be seen!",
        "{}",
        NULL,
        "I () be seen!"
    );
}

static void
test_interpolation_13(void)
{
    run(
        "failed context lookups should default to empty strings",
        "I ({{{cannot}}}) be seen!",
        "{}",
        NULL,
        "I () be seen!"
    );
}

static void
test_interpolation_14(void)
{
    run(
        "failed context lookups should default to empty strings",
        "I ({{&cannot}}) be seen!",
        "{}",
        NULL,
        "I () be seen!"
    );
}

static void
test_interpolation_15(void)
{
    run(
        "dotted names should be considered a form of shorthand for sections",
        "\"{{person.name}}\" == \"{{#person}}{{name}}{{/person}}\"",
        "{\"person\": {\"name\": \"Joe\"}}",
        NULL,
        "\"Joe\" == \"Joe\""
    );
}

static void
test_interpolation_16(void)
{
    run(
        "dotted names should be considered a form of shorthand for sections",
        "\"{{{person.name}}}\" == \"{{#person}}{{{name}}}{{/person}}\"",
        "{\"person\": {\"name\": \"Joe\"}}",
        NULL,
        "\"Joe\" == \"Joe\""
    );
}

static void
test_interpolation_17(void)
{
    run(
        "dotted names should be considered a form of shorthand for sections",
        "\"{{&person.name}}\" == \"{{#person}}{{&name}}{{/person}}\"",
        "{\"person\": {\"name\": \"Joe\"}}",
        NULL,
        "\"Joe\" == \"Joe\""
    );
}

static void
test_interpolation_18(void)
{
    run(
        "dotted names should be functional to any level of nesting",
        "\"{{a.b.c.d.e.name}}\" == \"Phil\"",
        "{\"a\": {\"b\": {\"c\": {\"d\": {\"e\": {\"name\": \"Phil\"}}}}}}",
        NULL,
        "\"Phil\" == \"Phil\""
    );
}

static void
test_interpolation_19(void)
{
    run(
        "any falsey value prior to the last part of the name should yield ''",
        "\"{{a.b.c}}\" == \"\"",
        "{\"a\": {}}",
        NULL,
        "\"\" == \"\""
    );
}

static void
test_interpolation_20(void)
{
    run(
        "each part of a dotted name should resolve only against its parent",
        "\"{{a.b.c.name}}\" == \"\"",
        "{\"a\": {\"b\": {}}, \"c\": {\"name\": \"Jim\"}}",
        NULL,
        "\"\" == \"\""
    );
}

static void
test_interpolation_21(void)
{
    run(
        "the first part of a dotted name should resolve as any other name",
        "\"{{#a}}{{b.c.d.e.name}}{{/a}}\" == \"Phil\"",
        "{\"a\": {\"b\": {\"c\": {\"d\": {\"e\": {\"name\": \"Phil\"}}}}}, \"b\": {\"c\": {\"d\": {\"e\": {\"name\": \"Wrong\"}}}}}",
        NULL,
        "\"Phil\" == \"Phil\""
    );
}

static void
test_interpolation_22(void)
{
    run(
        "interpolation should not alter surrounding whitespace",
        "| {{string}} |",
        "{\"string\": \"---\"}",
        NULL,
        "| --- |"
    );
}

static void
test_interpolation_23(void)
{
    run(
        "interpolation should not alter surrounding whitespace",
        "| {{{string}}} |",
        "{\"string\": \"---\"}",
        NULL,
        "| --- |"
    );
}

static void
test_interpolation_24(void)
{
    run(
        "interpolation should not alter surrounding whitespace",
        "| {{&string}} |",
        "{\"string\": \"---\"}",
        NULL,
        "| --- |"
    );
}

static void
test_interpolation_25(void)
{
    run(
        "standalone interpolation should not alter surrounding whitespace",
        "  {{string}}\n",
        "{\"string\": \"---\"}",
        NULL,
        "  ---\n"
    );
}

static void
test_interpolation_26(void)
{
    run(
        "standalone interpolation should not alter surrounding whitespace",
        "  {{{string}}}\n",
        "{\"string\": \"---\"}",
        NULL,
        "  ---\n"
    );
}

static void
test_interpolation_27(void)
{
    run(
        "standalone interpolation should not alter surrounding whitespace",
        "  {{&string}}\n",
        "{\"string\": \"---\"}",
        NULL,
        "  ---\n"
    );
}

static void
test_interpolation_28(void)
{
    run(
        "superfluous in-tag whitespace should be ignored",
        "|{{ string }}|",
        "{\"string\": \"---\"}",
        NULL,
        "|---|"
    );
}

static void
test_interpolation_29(void)
{
    run(
        "superfluous in-tag whitespace should be ignored",
        "|{{{ string }}}|",
        "{\"string\": \"---\"}",
        NULL,
        "|---|"
    );
}

static void
test_interpolation_30(void)
{
    run(
        "superfluous in-tag whitespace should be ignored",
        "|{{& string }}|",
        "{\"string\": \"---\"}",
        NULL,
        "|---|"
    );
}

static void
test_inverted_1(void)
{
    run(
        "falsey sections should have their contents rendered",
        "\"{{^boolean}}This should be rendered.{{/boolean}}\"",
        "{\"boolean\": false}",
        NULL,
        "\"This should be rendered.\""
    );
}

static void
test_inverted_2(void)
{
    run(
        "truthy sections should have their contents omitted",
        "\"{{^boolean}}This should not be rendered.{{/boolean}}\"",
        "{\"boolean\": true}",
        NULL,
        "\"\""
    );
}

static void
test_inverted_3(void)
{
    run(
        "objects and hashes should behave like truthy values",
        "\"{{^context}}Hi {{name}}.{{/context}}\"",
        "{\"context\": {\"name\": \"Joe\"}}",
        NULL,
        "\"\""
    );
}

static void
test_inverted_4(void)
{
    run(
        "lists should behave like truthy values",
        "\"{{^list}}{{n}}{{/list}}\"",
        "{\"list\": [{\"n\": 1}, {\"n\": 2}, {\"n\": 3}]}",
        NULL,
        "\"\""
    );
}

static void
test_inverted_5(void)
{
    run(
        "empty lists should behave like falsey values",
        "\"{{^list}}Yay lists!{{/list}}\"",
        "{\"list\": []}",
        NULL,
        "\"Yay lists!\""
    );
}

static void
test_inverted_6(void)
{
    run(
        "multiple inverted sections per template should be permitted",
        "{{^bool}}\n* first\n{{/bool}}\n* {{two}}\n{{^bool}}\n* third\n{{/bool}}\n",
        "{\"bool\": false, \"two\": \"second\"}",
        NULL,
        "* first\n* second\n* third\n"
    );
}

static void
test_inverted_7(void)
{
    run(
        "nested falsey sections should have their contents rendered",
        "| A {{^bool}}B {{^bool}}C{{/bool}} D{{/bool}} E |",
        "{\"bool\": false}",
        NULL,
        "| A B C D E |"
    );
}

static void
test_inverted_8(void)
{
    run(
        "nested truthy sections should be omitted",
        "| A {{^bool}}B {{^bool}}C{{/bool}} D{{/bool}} E |",
        "{\"bool\": true}",
        NULL,
        "| A  E |"
    );
}

static void
test_inverted_9(void)
{
    run(
        "failed context lookups should be considered falsey",
        "[{{^missing}}Cannot find key 'missing'!{{/missing}}]",
        "{}",
        NULL,
        "[Cannot find key 'missing'!]"
    );
}

static void
test_inverted_10(void)
{
    run(
        "dotted names should be valid for inverted section tags",
        "\"{{^a.b.c}}Not Here{{/a.b.c}}\" == \"\"",
        "{\"a\": {\"b\": {\"c\": true}}}",
        NULL,
        "\"\" == \"\""
    );
}

static void
test_inverted_11(void)
{
    run(
        "dotted names should be valid for inverted section tags",
        "\"{{^a.b.c}}Not Here{{/a.b.c}}\" == \"Not Here\"",
        "{\"a\": {\"b\": {\"c\": false}}}",
        NULL,
        "\"Not Here\" == \"Not Here\""
    );
}

static void
test_inverted_12(void)
{
    run(
        "dotted names that cannot be resolved should be considered falsey",
        "\"{{^a.b.c}}Not Here{{/a.b.c}}\" == \"Not Here\"",
        "{\"a\": {}}",
        NULL,
        "\"Not Here\" == \"Not Here\""
    );
}

static void
test_inverted_13(void)
{
    run(
        "inverted sections should not alter surrounding whitespace",
        " | {{^boolean}}    |   {{/boolean}} | \n",
        "{\"boolean\": false}",
        NULL,
        " |     |    | \n"
    );
}

static void
test_inverted_14(void)
{
    run(
        "inverted should not alter internal whitespace",
        " | {{^boolean}} {{! Important Whitespace }}\n {{/boolean}} | \n",
        "{\"boolean\": false}",
        NULL,
        " |  \n  | \n"
    );
}

static void
test_inverted_15(void)
{
    run(
        "single-line sections should not alter surrounding whitespace",
        " {{^boolean}}NO{{/boolean}}\n {{^boolean}}WAY{{/boolean}}\n",
        "{\"boolean\": false}",
        NULL,
        " NO\n WAY\n"
    );
}

static void
test_inverted_16(void)
{
    run(
        "standalone lines should be removed from the template",
        "| This Is\n{{^boolean}}\n|\n{{/boolean}}\n| A Line\n",
        "{\"boolean\": false}",
        NULL,
        "| This Is\n|\n| A Line\n"
    );
}

static void
test_inverted_17(void)
{
    run(
        "standalone indented lines should be removed from the template",
        "| This Is\n  {{^boolean}}\n|\n  {{/boolean}}\n| A Line\n",
        "{\"boolean\": false}",
        NULL,
        "| This Is\n|\n| A Line\n"
    );
}

static void
test_inverted_18(void)
{
    run(
        "\"\\r\\n\" should be considered a newline for standalone tags",
        "|\r\n{{^boolean}}\r\n{{/boolean}}\r\n|",
        "{\"boolean\": false}",
        NULL,
        "|\r\n|"
    );
}

static void
test_inverted_19(void)
{
    run(
        "standalone tags should not require a newline to precede them",
        "  {{^boolean}}\n^{{/boolean}}\n/",
        "{\"boolean\": false}",
        NULL,
        "^\n/"
    );
}

static void
test_inverted_20(void)
{
    run(
        "standalone tags should not require a newline to follow them",
        "^{{^boolean}}\n/\n  {{/boolean}}",
        "{\"boolean\": false}",
        NULL,
        "^\n/\n"
    );
}

static void
test_inverted_21(void)
{
    run(
        "superfluous in-tag whitespace should be ignored",
        "|{{^ boolean }}={{/ boolean }}|",
        "{\"boolean\": false}",
        NULL,
        "|=|"
    );
}

static void
test_partials_1(void)
{
    run(
        "the greater-than operator should expand to the named partial",
        "\"{{>text}}\"",
        "{}",
        "{\"text\": \"from partial\"}",
        "\"from partial\""
    );
}

static void
test_partials_2(void)
{
    run(
        "the empty string should be used when the named partial is not found",
        "\"{{>text}}\"",
        "{}",
        "{}",
        "\"\""
    );
}

static void
test_partials_3(void)
{
    run(
        "the greater-than operator should operate within the current context",
        "\"{{>partial}}\"",
        "{\"text\": \"content\"}",
        "{\"partial\": \"*{{text}}*\"}",
        "\"*content*\""
    );
}

static void
test_partials_4(void)
{
    run(
        "the greater-than operator should properly recurse",
        "{{>node}}",
        "{\"content\": \"X\", \"nodes\": [{\"content\": \"Y\", \"nodes\": []}]}",
        "{\"node\": \"{{content}}<{{#nodes}}{{>node}}{{/nodes}}>\"}",
        "X<Y<>>"
    );
}

static void
test_partials_5(void)
{
    run(
        "the greater-than operator should not alter surrounding whitespace",
        "| {{>partial}} |",
        "{}",
        "{\"partial\": \"\\t|\\t\"}",
        "|  |    |"
    );
}

static void
test_partials_6(void)
{
    run(
        "whitespace should be left untouched",
        "  {{data}}  {{> partial}}\n",
        "{\"data\": \"|\"}",
        "{\"partial\": \">\\n>\"}",
        "  |  >\n>\n"
    );
}

static void
test_partials_7(void)
{
    run(
        "\"\\r\\n\" should be considered a newline for standalone tags",
        "|\r\n{{>partial}}\r\n|",
        "{}",
        "{\"partial\": \">\"}",
        "|\r\n>|"
    );
}

static void
test_partials_8(void)
{
    run(
        "standalone tags should not require a newline to precede them",
        "  {{>partial}}\n>",
        "{}",
        "{\"partial\": \">\\n>\"}",
        "  >\n  >>"
    );
}

static void
test_partials_9(void)
{
    run(
        "standalone tags should not require a newline to follow them",
        ">\n  {{>partial}}",
        "{}",
        "{\"partial\": \">\\n>\"}",
        ">\n  >\n  >"
    );
}

static void
test_partials_10(void)
{
    run(
        "each line of the partial should be indented before rendering",
        "\\\n {{>partial}}\n/\n",
        "{\"content\": \"<\\n->\"}",
        "{\"partial\": \"|\\n{{{content}}}\\n|\\n\"}",
        "\\\n |\n <\n->\n |\n/\n"
    );
}

static void
test_partials_11(void)
{
    run(
        "superfluous in-tag whitespace should be ignored",
        "|{{> partial }}|",
        "{\"boolean\": true}",
        "{\"partial\": \"[]\"}",
        "|[]|"
    );
}

static void
test_sections_1(void)
{
    run(
        "truthy sections should have their contents rendered",
        "\"{{#boolean}}This should be rendered.{{/boolean}}\"",
        "{\"boolean\": true}",
        NULL,
        "\"This should be rendered.\""
    );
}

static void
test_sections_2(void)
{
    run(
        "falsey sections should have their contents omitted",
        "\"{{#boolean}}This should not be rendered.{{/boolean}}\"",
        "{\"boolean\": false}",
        NULL,
        "\"\""
    );
}

static void
test_sections_3(void)
{
    run(
        "objects and hashes should be pushed onto the context stack",
        "\"{{#context}}Hi {{name}}.{{/context}}\"",
        "{\"context\": {\"name\": \"Joe\"}}",
        NULL,
        "\"Hi Joe.\""
    );
}

static void
test_sections_4(void)
{
    run(
        "all elements on the context stack should be accessible",
        "{{#a}}\n{{one}}\n{{#b}}\n{{one}}{{two}}{{one}}\n{{#c}}\n{{one}}{{two}}{{three}}{{two}}{{one}}\n{{#d}}\n{{one}}{{two}}{{three}}{{four}}{{three}}{{two}}{{one}}\n{{#e}}\n{{one}}{{two}}{{three}}{{four}}{{five}}{{four}}{{three}}{{two}}{{one}}\n{{/e}}\n{{one}}{{two}}{{three}}{{four}}{{three}}{{two}}{{one}}\n{{/d}}\n{{one}}{{two}}{{three}}{{two}}{{one}}\n{{/c}}\n{{one}}{{two}}{{one}}\n{{/b}}\n{{one}}\n{{/a}}\n",
        "{\"a\": {\"one\": 1}, \"e\": {\"five\": 5}, \"d\": {\"four\": 4}, \"b\": {\"two\": 2}, \"c\": {\"three\": 3}}",
        NULL,
        "1\n121\n12321\n1234321\n123454321\n1234321\n12321\n121\n1\n"
    );
}

static void
test_sections_5(void)
{
    run(
        "lists should be iterated; list items should visit the context stack",
        "\"{{#list}}{{item}}{{/list}}\"",
        "{\"list\": [{\"item\": 1}, {\"item\": 2}, {\"item\": 3}]}",
        NULL,
        "\"123\""
    );
}

static void
test_sections_6(void)
{
    run(
        "empty lists should behave like falsey values",
        "\"{{#list}}Yay lists!{{/list}}\"",
        "{\"list\": []}",
        NULL,
        "\"\""
    );
}

static void
test_sections_7(void)
{
    run(
        "multiple sections per template should be permitted",
        "{{#bool}}\n* first\n{{/bool}}\n* {{two}}\n{{#bool}}\n* third\n{{/bool}}\n",
        "{\"bool\": true, \"two\": \"second\"}",
        NULL,
        "* first\n* second\n* third\n"
    );
}

static void
test_sections_8(void)
{
    run(
        "nested truthy sections should have their contents rendered",
        "| A {{#bool}}B {{#bool}}C{{/bool}} D{{/bool}} E |",
        "{\"bool\": true}",
        NULL,
        "| A B C D E |"
    );
}

static void
test_sections_9(void)
{
    run(
        "nested falsey sections should be omitted",
        "| A {{#bool}}B {{#bool}}C{{/bool}} D{{/bool}} E |",
        "{\"bool\": false}",
        NULL,
        "| A  E |"
    );
}

static void
test_sections_10(void)
{
    run(
        "failed context lookups should be considered falsey",
        "[{{#missing}}Found key 'missing'!{{/missing}}]",
        "{}",
        NULL,
        "[]"
    );
}

static void
test_sections_11(void)
{
    run(
        "implicit iterators should directly interpolate strings",
        "\"{{#list}}({{.}}){{/list}}\"",
        "{\"list\": [\"a\", \"b\", \"c\", \"d\", \"e\"]}",
        NULL,
        "\"(a)(b)(c)(d)(e)\""
    );
}

static void
test_sections_12(void)
{
    run(
        "implicit iterators should cast integers to strings and interpolate",
        "\"{{#list}}({{.}}){{/list}}\"",
        "{\"list\": [1, 2, 3, 4, 5]}",
        NULL,
        "\"(1)(2)(3)(4)(5)\""
    );
}

static void
test_sections_13(void)
{
    run(
        "implicit iterators should cast decimals to strings and interpolate",
        "\"{{#list}}({{.}}){{/list}}\"",
        "{\"list\": [1.1, 2.2, 3.3, 4.4, 5.5]}",
        NULL,
        "\"(1.1)(2.2)(3.3)(4.4)(5.5)\""
    );
}

static void
test_sections_14(void)
{
    run(
        "implicit iterators should allow iterating over nested arrays",
        "\"{{#list}}({{#.}}{{.}}{{/.}}){{/list}}\"",
        "{\"list\": [[1, 2, 3], [\"a\", \"b\", \"c\"]]}",
        NULL,
        "\"(123)(abc)\""
    );
}

static void
test_sections_15(void)
{
    run(
        "dotted names should be valid for section tags",
        "\"{{#a.b.c}}Here{{/a.b.c}}\" == \"Here\"",
        "{\"a\": {\"b\": {\"c\": true}}}",
        NULL,
        "\"Here\" == \"Here\""
    );
}

static void
test_sections_16(void)
{
    run(
        "dotted names should be valid for section tags",
        "\"{{#a.b.c}}Here{{/a.b.c}}\" == \"\"",
        "{\"a\": {\"b\": {\"c\": false}}}",
        NULL,
        "\"\" == \"\""
    );
}

static void
test_sections_17(void)
{
    run(
        "dotted names that cannot be resolved should be considered falsey",
        "\"{{#a.b.c}}Here{{/a.b.c}}\" == \"\"",
        "{\"a\": {}}",
        NULL,
        "\"\" == \"\""
    );
}

static void
test_sections_18(void)
{
    run(
        "sections should not alter surrounding whitespace",
        " | {{#boolean}}    |   {{/boolean}} | \n",
        "{\"boolean\": true}",
        NULL,
        " |     |    | \n"
    );
}

static void
test_sections_19(void)
{
    run(
        "sections should not alter internal whitespace",
        " | {{#boolean}} {{! Important Whitespace }}\n {{/boolean}} | \n",
        "{\"boolean\": true}",
        NULL,
        " |  \n  | \n"
    );
}

static void
test_sections_20(void)
{
    run(
        "single-line sections should not alter surrounding whitespace",
        " {{#boolean}}YES{{/boolean}}\n {{#boolean}}GOOD{{/boolean}}\n",
        "{\"boolean\": true}",
        NULL,
        " YES\n GOOD\n"
    );
}

static void
test_sections_21(void)
{
    run(
        "standalone lines should be removed from the template",
        "| This Is\n{{#boolean}}\n|\n{{/boolean}}\n| A Line\n",
        "{\"boolean\": true}",
        NULL,
        "| This Is\n|\n| A Line\n"
    );
}

static void
test_sections_22(void)
{
    run(
        "indented standalone lines should be removed from the template",
        "| This Is\n  {{#boolean}}\n|\n  {{/boolean}}\n| A Line\n",
        "{\"boolean\": true}",
        NULL,
        "| This Is\n|\n| A Line\n"
    );
}

static void
test_sections_23(void)
{
    run(
        "\"\\r\\n\" should be considered a newline for standalone tags",
        "|\r\n{{#boolean}}\r\n{{/boolean}}\r\n|",
        "{\"boolean\": true}",
        NULL,
        "|\r\n|"
    );
}

static void
test_sections_24(void)
{
    run(
        "standalone tags should not require a newline to precede them",
        "  {{#boolean}}\n#{{/boolean}}\n/",
        "{\"boolean\": true}",
        NULL,
        "#\n/"
    );
}

static void
test_sections_25(void)
{
    run(
        "standalone tags should not require a newline to follow them",
        "#{{#boolean}}\n/\n  {{/boolean}}",
        "{\"boolean\": true}",
        NULL,
        "#\n/\n"
    );
}

static void
test_sections_26(void)
{
    run(
        "superfluous in-tag whitespace should be ignored",
        "|{{# boolean }}={{/ boolean }}|",
        "{\"boolean\": true}",
        NULL,
        "|=|"
    );
}

TEST_LIST = {
    { "comments-1", test_comments_1 },
    { "comments-2", test_comments_2 },
    { "comments-3", test_comments_3 },
    { "comments-4", test_comments_4 },
    { "comments-5", test_comments_5 },
    { "comments-6", test_comments_6 },
    { "comments-7", test_comments_7 },
    { "comments-8", test_comments_8 },
    { "comments-9", test_comments_9 },
    { "comments-10", test_comments_10 },
    { "comments-11", test_comments_11 },
    { "delimiters-1", test_delimiters_1 },
    { "delimiters-2", test_delimiters_2 },
    { "delimiters-3", test_delimiters_3 },
    { "delimiters-4", test_delimiters_4 },
    { "delimiters-5", test_delimiters_5 },
    { "delimiters-6", test_delimiters_6 },
    { "delimiters-7", test_delimiters_7 },
    { "delimiters-8", test_delimiters_8 },
    { "delimiters-9", test_delimiters_9 },
    { "delimiters-10", test_delimiters_10 },
    { "delimiters-11", test_delimiters_11 },
    { "delimiters-12", test_delimiters_12 },
    { "delimiters-13", test_delimiters_13 },
    { "delimiters-14", test_delimiters_14 },
    { "interpolation-1", test_interpolation_1 },
    { "interpolation-2", test_interpolation_2 },
    { "interpolation-3", test_interpolation_3 },
    { "interpolation-4", test_interpolation_4 },
    { "interpolation-5", test_interpolation_5 },
    { "interpolation-6", test_interpolation_6 },
    { "interpolation-7", test_interpolation_7 },
    { "interpolation-8", test_interpolation_8 },
    { "interpolation-9", test_interpolation_9 },
    { "interpolation-10", test_interpolation_10 },
    { "interpolation-11", test_interpolation_11 },
    { "interpolation-12", test_interpolation_12 },
    { "interpolation-13", test_interpolation_13 },
    { "interpolation-14", test_interpolation_14 },
    { "interpolation-15", test_interpolation_15 },
    { "interpolation-16", test_interpolation_16 },
    { "interpolation-17", test_interpolation_17 },
    { "interpolation-18", test_interpolation_18 },
    { "interpolation-19", test_interpolation_19 },
    { "interpolation-20", test_interpolation_20 },
    { "interpolation-21", test_interpolation_21 },
    { "interpolation-22", test_interpolation_22 },
    { "interpolation-23", test_interpolation_23 },
    { "interpolation-24", test_interpolation_24 },
    { "interpolation-25", test_interpolation_25 },
    { "interpolation-26", test_interpolation_26 },
    { "interpolation-27", test_interpolation_27 },
    { "interpolation-28", test_interpolation_28 },
    { "interpolation-29", test_interpolation_29 },
    { "interpolation-30", test_interpolation_30 },
    { "inverted-1", test_inverted_1 },
    { "inverted-2", test_inverted_2 },
    { "inverted-3", test_inverted_3 },
    { "inverted-4", test_inverted_4 },
    { "inverted-5", test_inverted_5 },
    { "inverted-6", test_inverted_6 },
    { "inverted-7", test_inverted_7 },
    { "inverted-8", test_inverted_8 },
    { "inverted-9", test_inverted_9 },
    { "inverted-10", test_inverted_10 },
    { "inverted-11", test_inverted_11 },
    { "inverted-12", test_inverted_12 },
    { "inverted-13", test_inverted_13 },
    { "inverted-14", test_inverted_14 },
    { "inverted-15", test_inverted_15 },
    { "inverted-16", test_inverted_16 },
    { "inverted-17", test_inverted_17 },
    { "inverted-18", test_inverted_18 },
    { "inverted-19", test_inverted_19 },
    { "inverted-20", test_inverted_20 },
    { "inverted-21", test_inverted_21 },
    { "partials-1", test_partials_1 },
    { "partials-2", test_partials_2 },
    { "partials-3", test_partials_3 },
    { "partials-4", test_partials_4 },
    { "partials-5", test_partials_5 },
    { "partials-6", test_partials_6 },
    { "partials-7", test_partials_7 },
    { "partials-8", test_partials_8 },
    { "partials-9", test_partials_9 },
    { "partials-10", test_partials_10 },
    { "partials-11", test_partials_11 },
    { "sections-1", test_sections_1 },
    { "sections-2", test_sections_2 },
    { "sections-3", test_sections_3 },
    { "sections-4", test_sections_4 },
    { "sections-5", test_sections_5 },
    { "sections-6", test_sections_6 },
    { "sections-7", test_sections_7 },
    { "sections-8", test_sections_8 },
    { "sections-9", test_sections_9 },
    { "sections-10", test_sections_10 },
    { "sections-11", test_sections_11 },
    { "sections-12", test_sections_12 },
    { "sections-13", test_sections_13 },
    { "sections-14", test_sections_14 },
    { "sections-15", test_sections_15 },
    { "sections-16", test_sections_16 },
    { "sections-17", test_sections_17 },
    { "sections-18", test_sections_18 },
    { "sections-19", test_sections_19 },
    { "sections-20", test_sections_20 },
    { "sections-21", test_sections_21 },
    { "sections-22", test_sections_22 },
    { "sections-23", test_sections_23 },
    { "sections-24", test_sections_24 },
    { "sections-25", test_sections_25 },
    { "sections-26", test_sections_26 },
    { 0 }
};

