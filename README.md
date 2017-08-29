[![Build status (travis-ci.com)](https://img.shields.io/travis/mity/mustache4c/master.svg?label=linux%20build)](https://travis-ci.org/mity/mustache4c)
[![Build status (appveyor.com)](https://img.shields.io/appveyor/ci/mity/mustache4c/master.svg?label=windows%20build)](https://ci.appveyor.com/project/mity/mustache4c/branch/master)
[![Codecov](https://img.shields.io/codecov/c/github/mity/mustache4c/master.svg?label=code%20coverage)](https://codecov.io/github/mity/mustache4c)

# Mustache4C Readme

* Home: https://github.com/mity/mustache4c


## What is {{mustache}}

[{{mustache}}] is logic-less template system. The term "logic-less" means
it lacks any explicit `if` or `else` conditionals or `for` loops, however both
looping and conditional evaluation can be achieved using section tags
processing lists.

It is named {{mustache}} because of heavy use of curly braces (`{` and `}`)
which resemble a sideways mustache.

[{{mustache(5)}}][man5] can provide you an idea about {{mustache}} capabilities
and its syntax. More advanced use and understanding can be reached if you
immerse in [{{mustache}} official specification][spec].


## What is Mustache4C

Mustache4C is C implementation of {{mustache}} parser and processor,
intended mainly for embedding in other C/C++ code.

Note it does not enforce any explicit data structures for data. Many other
implementations assume some particular data storage (typically JSON) which is
ready and provided before the template processing begins.

Instead of that, Mustache4C asks the application for any data referred
from the template via some callback functions, and Mustache4C does not
assume anything but tree hierarchy of those data and that each node in the
tree may be represented with a single opaque pointer (`void*`).

This even allows the application to generate the data on the fly, or retrieve
it from a database instead of getting and preparing whole data set beforehand
in the computer memory.

A documentation of its API is currently only available directly in the [header
file `mustache.h`][mustache.h].


## Current Status

### Specification Conformance

Mustache4C is still in the development process but it is already becoming
useful. The current state of main features (using the terminology of the
[{{mustache}} specification 1.1.3][spec]) is as follows.

 * [x] **Variables** (`{{foo}}`, `{{{bar}}}`):
       Implemented. All [spec tests][spec-interpolation] are passing.

 * [x] **(Regular) Sections**  (`{{#foo}} ... {{/foo}}`):
       Implemented. All [spec tests][spec-sections] are passing.

 * [x] **Inverted Sections** (`{{^foo}} ... {{/foo}}`):
       Implemented. All [spec tests][spec-inverted] are passing.

 * [x] **Comments** (`{{! foo bar }}`):
       Implemented. All [spec tests][spec-comments] are passing.

 * [ ] **Partials** (`{{>foo}}`):
       Mostly implemented. Partials do not yet inherit their indentation
       from parent template. That's why three [spec tests][spec-partials] are
       failing.

 * [x] **Set Delimiter** (`{{=<% %>=}}`):
       Implemented. All [spec tests][spec-delimiters] are passing.

### Extensions

When seen as useful, some extensions may be implemented after the conformance
with the specification is reached.

### Note about Lambdas

The design of Mustache4C in general supports generating or retrieving some data
in run-time, which more or less allows application to support lambda. But such
implementation is and will be on the application using Mustache4C, and the
implementation likely shall be limited to things needed by the application.

Full lambda support as seen in many modern scripting languages is not and
will not be supported by Mustache4C.


## License

Mustache4C is covered with MIT license, see the file `LICENSE.md`.


## Reporting Bugs

If you encounter any bug, please be so kind and report it. Unheard bugs cannot
get fixed. You can submit bug reports here:

* https://github.com/mity/mustache4c/issues




[{{mustache}}]: https://mustache.github.io/
[man5]: https://mustache.github.io/mustache.5.html
[spec]: https://github.com/mustache/spec
[spec-comments]: https://github.com/mustache/spec/blob/master/specs/comments.yml
[spec-delimiters]: https://github.com/mustache/spec/blob/master/specs/delimiters.yml
[spec-interpolation]: https://github.com/mustache/spec/blob/master/specs/interpolationv.yml
[spec-inverted]: https://github.com/mustache/spec/blob/master/specs/inverted.yml
[spec-partials]: https://github.com/mustache/spec/blob/master/specs/partials.yml
[spec-sections]: https://github.com/mustache/spec/blob/master/specs/sections.yml
[spec-lambdas]: https://github.com/mustache/spec/blob/master/specs/~lambdas.yml
[mustache.h]: https://github.com/mity/mustache4c/blob/master/src/mustache.h
