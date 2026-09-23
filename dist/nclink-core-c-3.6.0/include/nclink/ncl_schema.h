/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - JSON Schema validation.
 *
 * A device describes its tool methods with JSON Schema; this module validates a
 * request against the schema and collects one message per violation. It
 * implements the draft-07 subset that the NC-Link method schemas actually use:
 *
 *   type (single or array)     enum  const
 *   properties  required  additionalProperties  minProperties  maxProperties
 *   items (schema or tuple)  additionalItems  minItems  maxItems  uniqueItems
 *   minLength  maxLength  pattern   (ECMA-flavoured regex, see below)
 *   minimum  maximum  exclusiveMinimum  exclusiveMaximum  multipleOf
 *   allOf  anyOf  oneOf  not  if/then/else
 *   $ref ("#/definitions/x", "#/$defs/x", "#/properties/x", ...)  format
 *
 * `pattern` is implemented by a small backtracking regular expression engine
 * (literals, '.', classes, groups, alternation, anchors and the usual
 * quantifiers). Because the engine only has to answer "does it match
 * somewhere", lazy quantifiers are treated as greedy.
 *
 * Error strings are shaped "#/name: expected type: String, found: Integer", so
 * log lines stay readable; the exact wording is not part of the protocol
 * contract.
 */
#ifndef NCL_SCHEMA_H
#define NCL_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Deepest $ref hop before validation gives up (cycle guard). */
#define NCL_SCHEMA_MAX_REF_DEPTH 32

typedef struct ncl_schema ncl_schema;

/**
 * Parse and prepare @p schema. Returns NULL and sets *error (heap, optional)
 * when the document is not a usable schema.
 */
ncl_schema *ncl_schema_compile(const ncl_json *schema, char **error);
ncl_schema *ncl_schema_compile_text(const char *text, size_t len, char **error);

void ncl_schema_free(ncl_schema *schema);

/** The schema document the object was compiled from (borrowed). */
const ncl_json *ncl_schema_root(const ncl_schema *schema);

/**
 * Validate @p value against @p schema, appending one message per violation to
 * @p errors, which is never cleared first; initialize it at the start of a
 * validation run. An empty error list means the document is valid.
 */
ncl_err ncl_schema_validate(const ncl_schema *schema, const ncl_json *value,
                            ncl_strvec *errors);

/** Parse @p text then validate it. Returns NCL_ERR_PARSE when it is not JSON. */
ncl_err ncl_schema_validate_text(const ncl_schema *schema, const char *text,
                                 size_t len, ncl_strvec *errors);

/**
 * Validate a JSON document given as text against a schema given as text. A top
 * level array validates against the schema as an array, anything else as a
 * value.
 */
ncl_err ncl_json_schema_validate(const char *json_text, const char *schema_text,
                                 ncl_strvec *errors);

/**
 * Render a message list as "[msg1, msg2]". Returns a heap string ("" when the
 * list is empty).
 */
char *ncl_schema_join_errors(const ncl_strvec *errors);

/* --------------------------------------------------------------- patterns -- */

/** Compiled regular expression (JSON Schema `pattern`). */
typedef struct ncl_regex ncl_regex;

/** Compile an ECMA-style pattern; NULL with *error set when unsupported. */
ncl_regex *ncl_regex_compile(const char *pattern, char **error);
void       ncl_regex_free(ncl_regex *regex);

/** True when @p regex matches anywhere inside @p text (unanchored search). */
bool ncl_regex_search(const ncl_regex *regex, const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NCL_SCHEMA_H */
