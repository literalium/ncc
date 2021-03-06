/* Copyright (c) 2018 Charles E. Youse (charles@gnuless.org). 
   All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include <string.h>
#include <stdlib.h>
#include "ncc1.h"

/* return the string table entry associated with a string 
   containing 'length' bytes at 'data', creating one if necessary. */

static struct string * string_buckets[NR_STRING_BUCKETS];

struct string *
stringize(char * data, int length)
{
    struct string *  string;
    struct string ** stringp;
    unsigned         hash;
    int              i;

    for (i = 0, hash = 0; i < length; i++) {
        hash <<= 4;
        hash ^= (data[i] & 0xff);
    }

    i = hash % NR_STRING_BUCKETS;
    for (stringp = &(string_buckets[i]); (string = *stringp); stringp = &((*stringp)->link)) {
        if (string->length != length) continue;
        if (string->hash != hash) continue;
        if (memcmp(string->data, data, length)) continue;

        *stringp = string->link; 
        string->link = string_buckets[i];
        string_buckets[i] = string;

        return string; 
    }

    string = (struct string *) allocate(sizeof(struct string));
    string->link = string_buckets[i];
    string_buckets[i] = string;
    string->hash = hash;
    string->length = length;
    string->asm_label = 0;
    string->token = KK_IDENT;

    string->data = allocate(length + 1);
    memcpy(string->data, data, length);
    string->data[length] = 0;

    return string;
}

/* walk the string table and output all the pending string literals. */

void
literals(void)
{
    struct string * string;
    int             i;

    for (i = 0; i < NR_STRING_BUCKETS; i++) 
        for (string = string_buckets[i]; string; string = string->link)
            if (string->asm_label) {
                segment(SEGMENT_TEXT);
                output("%L:\n", string->asm_label);
                output_string(string, string->length + 1);
            }
}

/* walk the symbol table and output directives for all undefined externs */

static void
externs1(struct symbol * symbol)
{
    if ((symbol->ss & S_EXTERN) && (symbol->ss & S_REFERENCED))
        output(".global %G\n", symbol);
}

void
externs(void)
{
    walk_symbols(SCOPE_GLOBAL, SCOPE_GLOBAL, externs1);
}

/* return the symbol table entry associated with a string literal. */

struct symbol *
string_symbol(struct string * string)
{
    struct symbol * symbol;
    struct type *   type;

    type = splice_types(new_type(T_ARRAY), new_type(T_CHAR));
    type->nr_elements = string->length + 1;
    if (string->asm_label == 0) string->asm_label = next_asm_label++;
    symbol = new_symbol(NULL, S_STATIC, type);
    symbol->i = string->asm_label;
    put_symbol(symbol, current_scope);
    return symbol;
}

/* the symbol table borrows the hash from the symbol identifiers 
   to use for its own purposes. since anonymous symbols are possible,
   there's an extra bucket just for them - putting them in the main
   table would serve no purpose, since we never find them by name. */

static struct symbol * symbol_buckets[NR_SYMBOL_BUCKETS + 1];

#define EXTRA_BUCKET        NR_SYMBOL_BUCKETS 
#define SYMBOL_BUCKET(id)   ((id) ? ((id)->hash % NR_SYMBOL_BUCKETS) : EXTRA_BUCKET)

/* allocate a new symbol. if 'type' is supplied, 
   the caller yields ownership. */

struct symbol *
new_symbol(struct string * id, int ss, struct type * type)
{
    struct symbol * symbol;

    symbol = (struct symbol *) allocate(sizeof(struct symbol));

    symbol->id = id;
    symbol->ss = ss;
    symbol->type = type;
    symbol->scope = SCOPE_NONE;
    symbol->reg = R_NONE;
    symbol->align = 0;
    symbol->target = NULL;
    symbol->link = NULL;
    symbol->input_name = NULL;
    symbol->line_number = 0;
    symbol->i = 0;
    symbol->list = NULL;

    return symbol;
}

/* put the symbol in the symbol table at the specified scope level. */

void
put_symbol(struct symbol * symbol, int scope)
{
    struct symbol ** bucketp;

    symbol->scope = scope;
    bucketp = &symbol_buckets[SYMBOL_BUCKET(symbol->id)];

    while (*bucketp && ((*bucketp)->scope > symbol->scope)) 
        bucketp = &((*bucketp)->link);

    symbol->link = *bucketp;
    *bucketp = symbol;
}

/* remove the symbol from the symbol table. */

static void
get_symbol(struct symbol * symbol)
{
    struct symbol ** bucketp;

    bucketp = &symbol_buckets[SYMBOL_BUCKET(symbol->id)];
    while (*bucketp != symbol) bucketp = &((*bucketp)->link);
    *bucketp = symbol->link;
}

/* put the symbol on the end of the list. */

void
put_symbol_list(struct symbol * symbol, struct symbol ** list)
{
    while (*list) list = &((*list)->list);
    *list = symbol;
}

/* find a symbol on the list. returns NULL if not found. */

struct symbol *
find_symbol_list(struct string * id, struct symbol ** list)
{
    struct symbol * symbol = *list;

    while (symbol && (symbol->id != id)) symbol = symbol->list;
    return symbol;
}

/* if 'id' is a typedef'd name visible in the current scope,
   return its symbol, otherwise NULL. note we can't just look
   for S_TYPEDEF directly, because that would skip names that
   hide the typedef. */

struct symbol *
find_typedef(struct string * id)
{
    struct symbol * symbol;

    symbol = find_symbol(id, S_NORMAL, SCOPE_GLOBAL, current_scope);

    if (symbol && (symbol->ss & S_TYPEDEF)) 
        return symbol;
    else
        return NULL;
}

/* find, or create, the label symbol */

struct symbol *
find_label(struct string * id)
{
    struct symbol * symbol;

    symbol = find_symbol(id, S_LABEL, SCOPE_FUNCTION, SCOPE_FUNCTION);
    if (symbol == NULL) {
        symbol = new_symbol(id, S_LABEL, NULL);
        symbol->target = new_block();
        put_symbol(symbol, SCOPE_FUNCTION);
    }
    return symbol;
}

/* find a symbol with the given 'id' in the namespace 'ss'
   between scopes 'start' and 'end' (inclusive). symbols marked 
   S_LURKER are ignored, unless S_LURKER is one of the flags 
   specified in 'ss'. returns NULL if no symbol found. */

struct symbol *
find_symbol(struct string * id, int ss, int start, int end)
{
    struct symbol * symbol;
    int             i;

    i = SYMBOL_BUCKET(id);
    for (symbol = symbol_buckets[i]; symbol; symbol = symbol->link) {
        if (symbol->ss & S_HIDDEN) continue;
        if (symbol->scope < start) break;
        if (symbol->scope > end) continue;
        if (symbol->id != id) continue;
        if ((symbol->ss & S_LURKER) && !(ss & S_LURKER)) continue;
        if ((symbol->ss & ss) == 0) continue;
        return symbol;
    }

    return NULL;
}

/* find a symbol by (pseudo) register. return NULL if not found. this 
   is very slow, because the symbol table isn't indexed by register. */

struct symbol *
find_symbol_by_reg(int reg)
{
    struct symbol * symbol;
    int             i;

    for (i = 0; i <= EXTRA_BUCKET; i++) 
        for (symbol = symbol_buckets[i]; symbol; symbol = symbol->link) 
            if (symbol->reg == reg) return symbol;

    return NULL;
}

/* someone's interested in the memory allocated to this symbol,
   so make sure it's allocated. */

void
store_symbol(struct symbol * symbol)
{
    if ((symbol->ss & S_BLOCK) && (symbol->i == 0)) {
        frame_offset += size_of(symbol->type);
        frame_offset = ROUND_UP(frame_offset, align_of(symbol->type));
        symbol->i = -frame_offset;
    }
}

/* return the pseudo register associated with the symbol, allocating
   one of appropriate type, if necessary. */

int
symbol_reg(struct symbol * symbol)
{
    if (symbol->reg != R_NONE) return symbol->reg;

    if (symbol->type->ts & (T_IS_INTEGRAL | T_PTR)) 
        symbol->reg = next_iregister++;
    else if (symbol->type->ts & T_IS_FLOAT)
        symbol->reg = next_fregister++;
    else error(ERROR_INTERNAL);

    return symbol->reg;
}

/* create an anonymous temporary symbol of the given type.
   these are always S_REGISTER (because the compiler uses them
   internally and will never take their addresses) and always 
   SCOPE_RETIRED (because they were never in scope to begin with).

   caller yields ownership of 'type'. */

struct symbol *
temporary_symbol(struct type * type)
{
    struct symbol * symbol;

    symbol = new_symbol(NULL, S_REGISTER, type);
    put_symbol(symbol, SCOPE_RETIRED);
    return symbol;
}

/* walk the symbol table between scopes 'start' and 'end', inclusive,
   calling f() on each one. the EXTRA_BUCKET is included in the traversal. */

void
walk_symbols(int start, int end, void (*f)(struct symbol *))
{
    struct symbol * symbol;
    struct symbol * link;
    int             i;

    for (i = 0; i <= EXTRA_BUCKET; i++) {
        for (symbol = symbol_buckets[i]; symbol; symbol = link) {
            link = symbol->link;
            if (symbol->scope < start) break;
            if (symbol->scope > end) continue;
            f(symbol);
        }
    }
}

/* entering a scope is trivial. */

void
enter_scope(void)
{
    ++current_scope;
    if (current_scope > SCOPE_MAX) error(ERROR_NESTING);
}

/* exiting a scope is called when exiting a block scope
   (EXIT_SCOPE_BLOCK) or when exiting a prototype scope
   (EXIT_SCOPE_PROTO). in both cases, the symbols are
   removed from view, it just depends how. */

static void
exit1(struct symbol * symbol)
{
    get_symbol(symbol);
    put_symbol(symbol, SCOPE_RETIRED);
}

static void
exit2(struct symbol * symbol)
{
    get_symbol(symbol);
    symbol->ss |= S_HIDDEN;
    put_symbol(symbol, current_scope - 1);
}

void
exit_scope(int mode)
{
    if (mode == EXIT_SCOPE_BLOCK)
        walk_symbols(current_scope, SCOPE_MAX, exit1);
    else
        walk_symbols(current_scope, SCOPE_MAX, exit2);

    --current_scope;
}

/* free a symbol and its resources, or a symbol list */

void
free_symbol(struct symbol * symbol)
{
    free_type(symbol->type);
    free(symbol);
}

void
free_symbol_list(struct symbol ** list)
{
    struct symbol * entry;

    while (entry = *list) {
        *list = entry->list;
        free_symbol(entry);
    }
}

/* after a function definition is completed, call free_symbols()
   to finally clear out all the out-of-scope symbols. */

static void
free_symbols1(struct symbol * symbol)
{
    get_symbol(symbol);
    free_symbol(symbol);
}

void
free_symbols(void)
{
    walk_symbols(SCOPE_FUNCTION, SCOPE_RETIRED, free_symbols1);
}

