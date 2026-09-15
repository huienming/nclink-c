/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - the small regular expression engine behind JSON Schema
 * `pattern`. Compiled to a program and run by a backtracking VM.
 *
 * Supported: literals, '.', character classes ("[a-z]", "[^0-9]", the usual
 * escapes incl. \d \D \w \W \s \S), groups "(...)", non capturing "(?:...)",
 * alternation "|", anchors "^" "\b" "\B" "$", and the quantifiers "*" "+" "?"
 * "{n}" "{n,}" "{n,m}". Lazy markers ("*?", "+?") are accepted and treated as
 * greedy: validation only asks whether the pattern matches at all, and the two
 * agree on that question.
 */
#include "nclink/ncl_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"

typedef enum {
    RE_CHAR = 0,
    RE_ANY,
    RE_CLASS,
    RE_SPLIT,
    RE_JMP,
    RE_BOL,
    RE_EOL,
    RE_WORDB,
    RE_NWORDB,
    RE_FAIL,
    RE_MATCH
} re_op;

typedef struct {
    re_op     op;
    unsigned char ch;      /**< RE_CHAR */
    unsigned char bitmap[32]; /**< RE_CLASS: 256 bit set */
    int       x;           /**< jump target / split branch A */
    int       y;           /**< split branch B */
} re_inst;

struct ncl_regex {
    re_inst *prog;
    size_t   len;
    size_t   cap;
    bool     anchored_start;
    bool     anchored_end;
};

/* ------------------------------------------------------------- pattern ---- */

typedef struct {
    const char *pattern;
    size_t      len;
    size_t      pos;
    ncl_regex  *regex;
    char      **error;
} re_parser;

static bool re_emit(ncl_regex *regex, re_op op, int *out_index)
{
    if (regex->len == regex->cap) {
        size_t cap = regex->cap == 0 ? 32 : regex->cap * 2;
        re_inst *grown = (re_inst *)realloc(regex->prog, cap * sizeof(re_inst));
        if (grown == NULL) {
            return false;
        }
        regex->prog = grown;
        regex->cap = cap;
    }
    memset(&regex->prog[regex->len], 0, sizeof(re_inst));
    regex->prog[regex->len].op = op;
    if (out_index != NULL) {
        *out_index = (int)regex->len;
    }
    regex->len++;
    return true;
}

static void re_fail(re_parser *p, const char *message)
{
    if (p->error != NULL && *p->error == NULL) {
        *p->error = ncl_strdup(message);
    }
}

static void re_class_set(unsigned char *bitmap, unsigned char from,
                         unsigned char to)
{
    unsigned value;
    for (value = from; value <= to && value < 256; value++) {
        bitmap[value / 8] = (unsigned char)(bitmap[value / 8] | (1u << (value % 8)));
    }
}

static void re_class_negate(unsigned char *bitmap)
{
    size_t i;
    for (i = 0; i < 32; i++) {
        bitmap[i] = (unsigned char)~bitmap[i];
    }
}

/** Expand a backslash escape into a class bitmap or a single character. */
static bool re_escape_class(char c, unsigned char *bitmap)
{
    switch (c) {
    case 'd':
        re_class_set(bitmap, '0', '9');
        return true;
    case 'D':
        re_class_set(bitmap, '0', '9');
        re_class_negate(bitmap);
        return true;
    case 'w':
        re_class_set(bitmap, '0', '9');
        re_class_set(bitmap, 'a', 'z');
        re_class_set(bitmap, 'A', 'Z');
        re_class_set(bitmap, '_', '_');
        return true;
    case 'W':
        re_class_set(bitmap, '0', '9');
        re_class_set(bitmap, 'a', 'z');
        re_class_set(bitmap, 'A', 'Z');
        re_class_set(bitmap, '_', '_');
        re_class_negate(bitmap);
        return true;
    case 's':
        re_class_set(bitmap, ' ', ' ');
        re_class_set(bitmap, '\t', '\r');
        return true;
    case 'S':
        re_class_set(bitmap, ' ', ' ');
        re_class_set(bitmap, '\t', '\r');
        re_class_negate(bitmap);
        return true;
    default:
        return false;
    }
}

static int re_escape_char(char c)
{
    switch (c) {
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case 'f':
        return '\f';
    case 'v':
        return '\v';
    case '0':
        return '\0';
    default:
        return (unsigned char)c;
    }
}

static bool re_parse_alternation(re_parser *p);

/** Parse one character class starting at the '[' (already consumed). */
static bool re_parse_class(re_parser *p)
{
    unsigned char bitmap[32];
    int index = -1;
    bool negated = false;

    memset(bitmap, 0, sizeof(bitmap));
    if (p->pos < p->len && p->pattern[p->pos] == '^') {
        negated = true;
        p->pos++;
    }
    if (p->pos < p->len && p->pattern[p->pos] == ']') {
        re_class_set(bitmap, ']', ']');
        p->pos++;
    }
    while (p->pos < p->len && p->pattern[p->pos] != ']') {
        int from = (unsigned char)p->pattern[p->pos++];

        if (from == '\\' && p->pos < p->len) {
            char escape = p->pattern[p->pos++];
            if (re_escape_class(escape, bitmap)) {
                continue;
            }
            from = re_escape_char(escape);
        }
        if (p->pos + 1 < p->len && p->pattern[p->pos] == '-' &&
            p->pattern[p->pos + 1] != ']') {
            int to;
            p->pos++;
            to = (unsigned char)p->pattern[p->pos++];
            if (to == '\\' && p->pos < p->len) {
                to = re_escape_char(p->pattern[p->pos++]);
            }
            if (from > to) {
                re_fail(p, "character class range is inverted");
                return false;
            }
            re_class_set(bitmap, (unsigned char)from, (unsigned char)to);
        } else {
            re_class_set(bitmap, (unsigned char)from, (unsigned char)from);
        }
    }
    if (p->pos >= p->len || p->pattern[p->pos] != ']') {
        re_fail(p, "unterminated character class");
        return false;
    }
    p->pos++; /* ']' */
    if (negated) {
        re_class_negate(bitmap);
    }
    if (!re_emit(p->regex, RE_CLASS, &index)) {
        re_fail(p, "out of memory");
        return false;
    }
    memcpy(p->regex->prog[index].bitmap, bitmap, sizeof(bitmap));
    return true;
}

static bool re_parse_atom(re_parser *p)
{
    char c;

    if (p->pos >= p->len) {
        return true;
    }
    c = p->pattern[p->pos];
    switch (c) {
    case '(':
        p->pos++;
        if (p->pos + 1 < p->len && p->pattern[p->pos] == '?' &&
            p->pattern[p->pos + 1] == ':') {
            p->pos += 2;
        }
        if (!re_parse_alternation(p)) {
            return false;
        }
        if (p->pos >= p->len || p->pattern[p->pos] != ')') {
            re_fail(p, "unbalanced group");
            return false;
        }
        p->pos++;
        return true;
    case '[':
        p->pos++;
        return re_parse_class(p);
    case '.':
        p->pos++;
        return re_emit(p->regex, RE_ANY, NULL);
    case '^':
        p->pos++;
        return re_emit(p->regex, RE_BOL, NULL);
    case '$':
        p->pos++;
        return re_emit(p->regex, RE_EOL, NULL);
    case '\\': {
        char escape;
        unsigned char bitmap[32];
        int index = -1;

        p->pos++;
        if (p->pos >= p->len) {
            re_fail(p, "trailing backslash");
            return false;
        }
        escape = p->pattern[p->pos++];
        if (escape == 'b') {
            return re_emit(p->regex, RE_WORDB, NULL);
        }
        if (escape == 'B') {
            return re_emit(p->regex, RE_NWORDB, NULL);
        }
        memset(bitmap, 0, sizeof(bitmap));
        if (re_escape_class(escape, bitmap)) {
            if (!re_emit(p->regex, RE_CLASS, &index)) {
                re_fail(p, "out of memory");
                return false;
            }
            memcpy(p->regex->prog[index].bitmap, bitmap, sizeof(bitmap));
            return true;
        }
        if (!re_emit(p->regex, RE_CHAR, &index)) {
            re_fail(p, "out of memory");
            return false;
        }
        p->regex->prog[index].ch = (unsigned char)re_escape_char(escape);
        return true;
    }
    default:
        p->pos++;
        {
            int index = -1;
            if (!re_emit(p->regex, RE_CHAR, &index)) {
                re_fail(p, "out of memory");
                return false;
            }
            p->regex->prog[index].ch = (unsigned char)c;
        }
        return true;
    }
}

/**
 * Detach the trailing instruction block [atom_start, len) so the quantifier
 * scaffolding can be laid out in front of it. @p atom_end - @p atom_start
 * instructions are copied out and the program is truncated back to
 * @p atom_start; the caller releases the returned buffer.
 */
static re_inst *re_take_tail(re_parser *p, size_t atom_start,
                             size_t *out_len)
{
    size_t count = p->regex->len - atom_start;
    re_inst *saved = (re_inst *)malloc(count * sizeof(re_inst));

    if (saved == NULL) {
        re_fail(p, "out of memory");
        return NULL;
    }
    memcpy(saved, &p->regex->prog[atom_start], count * sizeof(re_inst));
    p->regex->len = atom_start;
    *out_len = count;
    return saved;
}

/**
 * Append @p block back, relocating any jump target that pointed inside
 * [atom_start, atom_start + block_len] (the block itself plus the position
 * right after it).
 */
static bool re_append_block(re_parser *p, const re_inst *block, size_t block_len,
                            size_t atom_start, size_t *out_base)
{
    size_t base = p->regex->len;
    size_t i;

    for (i = 0; i < block_len; i++) {
        int index = -1;
        re_inst source = block[i];

        if (!re_emit(p->regex, source.op, &index)) {
            re_fail(p, "out of memory");
            return false;
        }
        p->regex->prog[index].ch = source.ch;
        memcpy(p->regex->prog[index].bitmap, source.bitmap,
               sizeof(source.bitmap));
        if (source.op == RE_SPLIT || source.op == RE_JMP) {
            if (source.x >= 0) {
                p->regex->prog[index].x =
                    (source.x >= (int)atom_start &&
                     source.x <= (int)(atom_start + block_len))
                        ? source.x - (int)atom_start + (int)base
                        : source.x;
            } else {
                p->regex->prog[index].x = source.x;
            }
            if (source.y >= 0) {
                p->regex->prog[index].y =
                    (source.y >= (int)atom_start &&
                     source.y <= (int)(atom_start + block_len))
                        ? source.y - (int)atom_start + (int)base
                        : source.y;
            } else {
                p->regex->prog[index].y = source.y;
            }
        }
    }
    *out_base = base;
    return true;
}

/**
 * Apply a quantifier to the atom block [atom_start, atom_end).
 *
 * The atom is detached, the scaffolding is written where it used to live and
 * the required copies are appended after it, so the control flow always enters
 * the loop through its SPLIT:
 *
 *   x?     SPLIT(x, after) x
 *   x*     loop: SPLIT(x, after) x JMP(loop)
 *   x+     x loop: SPLIT(x, after)
 *   x{n,m} x x ... [SPLIT(x, after) x] ... after:
 */
static bool re_parse_quantifier(re_parser *p, int atom_start, int atom_end)
{
    char c;
    long min = 0;
    long max = 1;
    size_t repeat_start = (size_t)atom_start;
    size_t repeat_len = 0;
    re_inst *atom;
    long i;
    bool ok = true;

    if (p->pos >= p->len) {
        return true;
    }
    c = p->pattern[p->pos];
    if (c == '*' || c == '+' || c == '?') {
        p->pos++;
        min = (c == '+') ? 1 : 0;
        max = (c == '?') ? 1 : -1;
    } else if (c == '{') {
        size_t scan = p->pos + 1;
        long value = 0;
        bool have_lower = false;

        if (scan >= p->len || p->pattern[scan] < '0' || p->pattern[scan] > '9') {
            return true; /* a literal '{' */
        }
        while (scan < p->len && p->pattern[scan] >= '0' &&
               p->pattern[scan] <= '9') {
            value = value * 10 + (p->pattern[scan] - '0');
            scan++;
            have_lower = true;
        }
        if (!have_lower) {
            return true;
        }
        min = value;
        if (scan < p->len && p->pattern[scan] == ',') {
            scan++;
            if (scan < p->len && p->pattern[scan] == '}') {
                max = -1;
            } else {
                max = 0;
                while (scan < p->len && p->pattern[scan] >= '0' &&
                       p->pattern[scan] <= '9') {
                    max = max * 10 + (p->pattern[scan] - '0');
                    scan++;
                }
            }
        } else {
            max = value;
        }
        if (scan >= p->len || p->pattern[scan] != '}') {
            return true; /* not a quantifier */
        }
        p->pos = scan + 1;
    } else {
        return true;
    }
    if (max >= 0 && max < min) {
        re_fail(p, "quantifier bounds are inverted");
        return false;
    }
    if (min > 1000 || (max > 1000)) {
        re_fail(p, "quantifier bound is too large");
        return false;
    }
    /* Lazy markers are accepted but ignored (greedy is enough for "matches?"). */
    if (p->pos < p->len && p->pattern[p->pos] == '?') {
        p->pos++;
    }

    (void)atom_end;
    atom = re_take_tail(p, repeat_start, &repeat_len);
    if (atom == NULL) {
        return false;
    }

    if (min == 0 && max == 1) {
        /* x? -> SPLIT(x, after) x */
        int split = -1;
        size_t base = 0;
        if (!re_emit(p->regex, RE_SPLIT, &split)) {
            re_fail(p, "out of memory");
            free(atom);
            return false;
        }
        if (!re_append_block(p, atom, repeat_len, repeat_start, &base)) {
            free(atom);
            return false;
        }
        p->regex->prog[split].x = (int)base;
        p->regex->prog[split].y = (int)p->regex->len;
    } else if (min == 0 && max < 0) {
        /* x* -> SPLIT(atom, after) atom JMP(back) */
        int split = -1;
        size_t base = 0;
        if (!re_emit(p->regex, RE_SPLIT, &split)) {
            re_fail(p, "out of memory");
            free(atom);
            return false;
        }
        if (!re_append_block(p, atom, repeat_len, repeat_start, &base)) {
            free(atom);
            return false;
        }
        if (!re_emit(p->regex, RE_JMP, NULL)) {
            re_fail(p, "out of memory");
            free(atom);
            return false;
        }
        p->regex->prog[split].x = (int)base;
        p->regex->prog[split].y = (int)p->regex->len;
        p->regex->prog[p->regex->len - 1].x = split;
    } else if (min == 1 && max < 0) {
        /* x+ -> atom SPLIT(atom, after) */
        int split = -1;
        size_t base = 0;
        if (!re_append_block(p, atom, repeat_len, repeat_start, &base)) {
            free(atom);
            return false;
        }
        if (!re_emit(p->regex, RE_SPLIT, &split)) {
            re_fail(p, "out of memory");
            free(atom);
            return false;
        }
        p->regex->prog[split].x = (int)base;
        p->regex->prog[split].y = (int)p->regex->len;
    } else {
        /* General {min,max}: min mandatory copies, then (max - min) optional
         * ones (or a loop when max is unbounded). */
        for (i = 0; i < min; i++) {
            size_t unused = 0;
            if (!re_append_block(p, atom, repeat_len, repeat_start, &unused)) {
                ok = false;
                break;
            }
        }
        if (ok && max < 0) {
            int split = -1;
            size_t base = 0;
            if (!re_emit(p->regex, RE_SPLIT, &split)) {
                re_fail(p, "out of memory");
                ok = false;
            } else if (!re_append_block(p, atom, repeat_len, repeat_start,
                                        &base)) {
                ok = false;
            } else if (!re_emit(p->regex, RE_JMP, NULL)) {
                re_fail(p, "out of memory");
                ok = false;
            } else {
                p->regex->prog[split].x = (int)base;
                p->regex->prog[split].y = (int)p->regex->len;
                p->regex->prog[p->regex->len - 1].x = split;
            }
        } else if (ok) {
            size_t optional_count = (size_t)(max - min);
            size_t after =
                p->regex->len + optional_count * (repeat_len + 1);
            size_t copy;
            for (copy = 0; copy < optional_count && ok; copy++) {
                int split = -1;
                size_t base = 0;
                if (!re_emit(p->regex, RE_SPLIT, &split)) {
                    re_fail(p, "out of memory");
                    ok = false;
                    break;
                }
                if (!re_append_block(p, atom, repeat_len, repeat_start, &base)) {
                    ok = false;
                    break;
                }
                p->regex->prog[split].x = (int)base;
                p->regex->prog[split].y = (int)after;
            }
        }
    }
    free(atom);
    return ok;
}

static bool re_parse_concat(re_parser *p)
{
    while (p->pos < p->len) {
        int atom_start;
        int atom_end;
        char c = p->pattern[p->pos];

        if (c == '|' || c == ')') {
            return true;
        }
        atom_start = (int)p->regex->len;
        if (!re_parse_atom(p)) {
            return false;
        }
        atom_end = (int)p->regex->len;
        if (atom_end == atom_start) {
            return true;
        }
        if (p->pos < p->len) {
            char q = p->pattern[p->pos];
            if (q == '*' || q == '+' || q == '?' || q == '{') {
                if (!re_parse_quantifier(p, atom_start, atom_end)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool re_parse_alternation(re_parser *p)
{
    size_t *jumps = NULL;
    size_t jump_count = 0;
    size_t jump_cap = 0;
    int split = -1;
    int fail_index = -1;
    size_t branch_start;
    size_t i;
    bool ok = true;

    /* Layout for "A|B|C":
     *   s0: SPLIT(A0, s1)
     *   A0: A
     *       JMP(AFTER)
     *   s1: SPLIT(B0, s2)
     *   B0: B
     *       JMP(AFTER)
     *   s2: SPLIT(C0, FAIL)
     *   C0: C
     *       JMP(AFTER)
     *   FAIL:
     *   AFTER:
     * A split whose second target is exhausted must fail the whole alternation,
     * hence the explicit FAIL sink; falling through to whatever follows the
     * group would wrongly accept a non-matching input.
     */
    if (!re_emit(p->regex, RE_SPLIT, &split)) {
        re_fail(p, "out of memory");
        return false;
    }
    branch_start = p->regex->len;
    p->regex->prog[split].x = (int)branch_start;

    for (;;) {
        if (!re_parse_concat(p)) {
            ok = false;
            break;
        }
        if (p->pos >= p->len || p->pattern[p->pos] != '|') {
            break;
        }
        p->pos++; /* '|' */
        if (!re_emit(p->regex, RE_JMP, NULL)) {
            re_fail(p, "out of memory");
            ok = false;
            break;
        }
        if (jump_count == jump_cap) {
            size_t cap = jump_cap == 0 ? 4 : jump_cap * 2;
            size_t *grown = (size_t *)realloc(jumps, cap * sizeof(size_t));
            if (grown == NULL) {
                re_fail(p, "out of memory");
                ok = false;
                break;
            }
            jumps = grown;
            jump_cap = cap;
        }
        jumps[jump_count++] = p->regex->len - 1;
        {
            int next_split = -1;
            if (!re_emit(p->regex, RE_SPLIT, &next_split)) {
                re_fail(p, "out of memory");
                ok = false;
                break;
            }
            /* The branch just parsed failed: fall through to the next split. */
            p->regex->prog[split].y = next_split;
            split = next_split;
            p->regex->prog[split].x = (int)p->regex->len;
        }
    }
    if (ok) {
        /* -1 means "no more alternatives": ncl_regex_compile() patches every
         * such target to the shared FAIL sink that lives after MATCH, so the
         * fall-through path is never accidentally rejected. */
        (void)fail_index;
        p->regex->prog[split].y = -1;
        for (i = 0; i < jump_count; i++) {
            p->regex->prog[jumps[i]].x = (int)p->regex->len; /* past FAIL */
        }
    }
    free(jumps);
    return ok;
}

/* ------------------------------------------------------------------ VM ---- */

typedef struct {
    const char     *text;
    size_t          len;
    const re_inst  *prog;
    size_t          prog_len;
    long long       budget;
} re_vm;

static bool re_is_word(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

static bool re_run(re_vm *vm, size_t pc, size_t sp)
{
    if (vm->budget-- <= 0) {
        return false;
    }
    if (pc >= vm->prog_len) {
        return false;
    }
    switch (vm->prog[pc].op) {
    case RE_CHAR:
        if (sp < vm->len &&
            (unsigned char)vm->text[sp] == vm->prog[pc].ch) {
            return re_run(vm, pc + 1, sp + 1);
        }
        return false;
    case RE_ANY:
        if (sp < vm->len && vm->text[sp] != '\n') {
            return re_run(vm, pc + 1, sp + 1);
        }
        return false;
    case RE_CLASS:
        if (sp < vm->len) {
            unsigned char c = (unsigned char)vm->text[sp];
            if ((vm->prog[pc].bitmap[c / 8] & (1u << (c % 8))) != 0) {
                return re_run(vm, pc + 1, sp + 1);
            }
        }
        return false;
    case RE_SPLIT:
        if (re_run(vm, (size_t)vm->prog[pc].x, sp)) {
            return true;
        }
        return re_run(vm, (size_t)vm->prog[pc].y, sp);
    case RE_JMP:
        return re_run(vm, (size_t)vm->prog[pc].x, sp);
    case RE_BOL:
        if (sp == 0) {
            return re_run(vm, pc + 1, sp);
        }
        return false;
    case RE_EOL:
        if (sp == vm->len) {
            return re_run(vm, pc + 1, sp);
        }
        return false;
    case RE_WORDB: {
        bool before = sp > 0 && re_is_word((unsigned char)vm->text[sp - 1]);
        bool after = sp < vm->len && re_is_word((unsigned char)vm->text[sp]);
        return before != after ? re_run(vm, pc + 1, sp) : false;
    }
    case RE_NWORDB: {
        bool before = sp > 0 && re_is_word((unsigned char)vm->text[sp - 1]);
        bool after = sp < vm->len && re_is_word((unsigned char)vm->text[sp]);
        return before == after ? re_run(vm, pc + 1, sp) : false;
    }
    case RE_FAIL:
        return false;
    case RE_MATCH:
        return true;
    default:
        return false;
    }
}

ncl_regex *ncl_regex_compile(const char *pattern, char **error)
{
    re_parser parser;
    ncl_regex *regex;
    int match_index = -1;

    if (error != NULL) {
        *error = NULL;
    }
    if (pattern == NULL) {
        return NULL;
    }
    regex = (ncl_regex *)calloc(1, sizeof(*regex));
    if (regex == NULL) {
        return NULL;
    }
    memset(&parser, 0, sizeof(parser));
    parser.pattern = pattern;
    parser.len = strlen(pattern);
    parser.regex = regex;
    parser.error = error;

    if (!re_parse_alternation(&parser) ||
        (parser.pos < parser.len && parser.pattern[parser.pos] == ')')) {
        ncl_regex_free(regex);
        return NULL;
    }
    if (parser.pos < parser.len) {
        re_fail(&parser, "unexpected trailing pattern characters");
        ncl_regex_free(regex);
        return NULL;
    }
    if (!re_emit(regex, RE_MATCH, &match_index)) {
        re_fail(&parser, "out of memory");
        ncl_regex_free(regex);
        return NULL;
    }
    /* Shared failure sink: any SPLIT target left at -1 (an exhausted
     * alternation) points here, which is outside the fall-through path. */
    {
        int fail_index = -1;
        size_t i;
        if (!re_emit(regex, RE_FAIL, &fail_index)) {
            re_fail(&parser, "out of memory");
            ncl_regex_free(regex);
            return NULL;
        }
        for (i = 0; i < regex->len; i++) {
            if (regex->prog[i].op == RE_SPLIT) {
                if (regex->prog[i].x < 0) {
                    regex->prog[i].x = fail_index;
                }
                if (regex->prog[i].y < 0) {
                    regex->prog[i].y = fail_index;
                }
            }
        }
    }
    regex->anchored_start = regex->len > 0 && regex->prog[0].op == RE_BOL;
    return regex;
}

void ncl_regex_free(ncl_regex *regex)
{
    if (regex == NULL) {
        return;
    }
    free(regex->prog);
    free(regex);
}

bool ncl_regex_search(const ncl_regex *regex, const char *text, size_t len)
{
    re_vm vm;
    size_t start;

    if (regex == NULL || text == NULL) {
        return false;
    }
    vm.text = text;
    vm.len = len;
    vm.prog = regex->prog;
    vm.prog_len = regex->len;
    vm.budget = 2000000;
    for (start = 0; start <= len; start++) {
        /* A leading '^' can only match at the beginning. */
        if (start > 0 && regex->anchored_start) {
            return false;
        }
        if (re_run(&vm, 0, start)) {
            return true;
        }
    }
    return false;
}
