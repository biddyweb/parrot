/*

=head1 NAME

pge/pge_gen.c - Generate PIR code from a P6 rule expression

=head1 DESCRIPTION

This file contains the functions designed to convert a P6 rule
expression (usually generated by pge_parse() ) into the PIR code
that can execute the rule on a string.

=head2 Functions

=over 4

=cut

*/

#include "pge.h"
#include "parrot/parrot.h"
#include <stdarg.h>
#include <stdlib.h>


static char* pge_cbuf = 0;
static int pge_cbuf_size = 0;
static int pge_cbuf_len = 0;
static int pge_cbuf_lcount = 0;
static int pge_istraced = 0;

static void pge_gen_exp(PGE_Exp* e, const char* succ);

/* emit(...) writes strings to an automatically grown string buffer */
static void
emit(const char* fmt, ...)
{
    int lookahead;
    va_list ap;
    lookahead = pge_cbuf_len + PGE_MAX_LITERAL_LEN * 2 + 3 + strlen(fmt) * 2;
    if (lookahead > pge_cbuf_size) {
        while (lookahead > pge_cbuf_size) pge_cbuf_size += 4096;
        pge_cbuf = realloc(pge_cbuf, pge_cbuf_size);
    }
    va_start(ap, fmt);
    pge_cbuf_len += vsprintf(pge_cbuf + pge_cbuf_len, fmt, ap);
    va_end(ap);
}


static void
emitlcount(void)
{
    char* s;
    int lcount = 0;

    for(s = pge_cbuf; *s; s++) { if (*s == '\n') lcount++; }
    if (lcount > pge_cbuf_lcount + 10) {
        emit("# line %d\n", lcount);
        pge_cbuf_lcount = lcount;
    }
}
        

static void
emitsub(const char* sub, ...)
{
    char* s[10];
    int i;
    va_list ap;

    va_start(ap, sub);
    for(i = 0; i < 10; i++) {
        s[i] = va_arg(ap, char*);
        if (!s[i]) break;
        emit("    save %s\n", s[i]);
    }
    va_end(ap);
    emit("    bsr %s\n", sub);
    while (i > 0) emit("    restore %s\n", s[--i]);
    emit("    if cutgrp goto fail_group\n");
}


/* str_con(...) converts string values into PIR string constants */
static char*
str_con(const unsigned char* s, int len)
{
    static char esc[PGE_MAX_LITERAL_LEN * 2 + 3];
    char* t = esc;
    int i;
    *(t++) = '"';
    for(i = 0; i < len; i++) {
        switch (s[i]) {
            case '\\': *(t++) = '\\'; *(t++) = '\\'; break;
            case '"' : *(t++) = '\\'; *(t++) = '"'; break;
            case '\'' : *(t++) = '\\'; *(t++) = '\''; break;
            case '\n': *(t++) = '\\'; *(t++) = 'n'; break;
            case '\r': *(t++) = '\\'; *(t++) = 'r'; break;
            case '\t': *(t++) = '\\'; *(t++) = 't'; break;
            case '\0': *(t++) = '\\'; *(t++) = '0'; break;
            default  : *(t++) = s[i]; break;
        }
    }
    *(t++) = '"';
    *t = 0;
    return esc;
}


/* return a quantifier as a <m..n> string */
static char*
fmt_quant(PGE_Exp* e)
{
    static char q[26];
    char c = (e->isgreedy) ? ' ' : '?';

    if (e->max == PGE_INF) sprintf(q, "<%d...>%c", e->min, c);
    else if (e->max != e->min) sprintf(q, "<%d..%d>%c", e->min, e->max, c);
    else sprintf(q, "<%d>%c", e->min, c);
    return q;
}


/* include calls to trace execution of the match */
static void
trace(const char* fmt, ...)
{
    static char s[80];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    emit("    # %s\n", s);
    if (pge_istraced) emit("    .trace(pos, '%s')\n", s);
}


static void
pge_gen_pattern_end(PGE_Exp* e, const char* succ)
{
    trace("eop");

    emit("    .yield(pos)\n");
    emit("    goto fail\n\n");
}


static void
pge_gen_dot(PGE_Exp* e, const char* succ)
{
    trace("dot %s", fmt_quant(e));

    emit("    maxrep = length target\n");
    emit("    maxrep -= pos\n");
    if (e->min > 0) emit("    if maxrep < %d goto fail\n", e->min);
    if (e->min == e->max) {
        emit("    pos += %d\n", e->min);
        emit("    goto %s\n\n", succ);
        return;
    }
    if (e->max != PGE_INF) {
        emit("    if maxrep <= %d goto R%d_1\n", e->max, e->id);
        emit("    maxrep = %d\n", e->max);
        emit("  R%d_1:\n", e->id);
    }
    if (e->isgreedy) {
        emit("    rep = maxrep\n");
        emit("    pos += rep\n");
        emit("  R%d_2:\n", e->id);
        emit("    if rep < %d goto fail\n", e->min);
        if (e->iscut) { emit("    goto %s\n", succ); return; }
        emit("    if rep == %d goto %s\n", e->min, succ);
        emitsub(succ, "pos", "rep", 0);
        emit("    dec rep\n");
        emit("    dec pos\n");
        emit("    goto R%d_2\n\n", e->id);
        return;
    }
    else { /* dot lazy */
        emit("    rep = %d\n", e->min);
        if (e->min > 0) emit("    pos += %d\n", e->min);
        emit("  R%d_3:\n", e->id);
        emit("    if rep > maxrep goto fail\n");
        if (e->iscut) { emit("    goto %s\n", succ); return; }
        emit("    if rep == maxrep goto %s\n", succ);
        emitsub(succ, "pos", "rep", "maxrep", 0);
        emit("    inc rep\n");
        emit("    inc pos\n");
        emit("    goto R%d_3\n\n", e->id);
        return;
    }
}


/*  pge_gen_string() handles cases where we have a repeating string
    value that won't change over the course of the repeat-- e.g.,
    literals and backreferences.  By the time we get here, the Rnnnn
    subroutine has already been started, and the PMC variables
    "str" and "strlen" have been set with the string to be (repeatedly)
    matched.  */
static void
pge_gen_string(PGE_Exp* e, const char* succ)
{
    if (e->min==1 && e->max==1) {
        emit("    substr $S0, target, pos, strlen\n");
        emit("    if $S0 != str goto fail\n");
        emit("    pos += strlen\n");
        emit("    goto %s\n\n", succ);
        return;
    }

    if (e->isgreedy) {
        emit("    rep = 0\n");
        emit("  R%d_1:\n", e->id);
        if (e->max != PGE_INF)
            emit("    if rep >= %d goto R%d_2\n", e->max, e->id);
        emit("    substr $S0, target, pos, strlen\n");
        emit("    if $S0 != str goto R%d_2\n", e->id);
        emit("    inc rep\n");
        emit("    pos += strlen\n", e->nlen);
        emit("    goto R%d_1\n", e->id);
        emit("  R%d_2:\n", e->id);
        if (e->min > 0) emit("    if rep < %d goto fail\n", e->min);
        if (e->iscut) { emit("    goto %s\n", succ); return; }
        emit("    if rep == %d goto %s\n", e->min, succ);
        emitsub(succ, "pos", "rep", "strlen", 0);
        emit("    dec rep\n");
        emit("    pos -= strlen\n");
        emit("    goto R%d_2\n\n", e->id);
        return;
    } 
    else { /* islazy */
        emit("    rep = 0\n");
        emit("  R%d_1:\n", e->id);
        if (e->min > 0)
            emit("    if rep < %d goto R%d_2:\n", e->min, e->id);
        if (e->iscut) { emit("    goto %s\n", succ); return; }
        if (e->max != PGE_INF) 
            emit("    if rep == %d goto %s\n", e->max, succ);
        emitsub(succ, "pos", "rep", "str", "strlen", 0);
        emit("  R%d_2:\n", e->id);
        emit("    substr $S0, target, pos, strlen\n", e->nlen);
        emit("    if $S0 != str goto fail\n");
        emit("    inc rep\n");
        emit("    pos += strlen\n");
        emit("    goto R%d_1\n\n", e->id);
        return;
    } 
}


static void
pge_gen_literal(PGE_Exp* e, const char* succ)
{
    trace("%.16s %s", str_con(e->name, e->nlen), fmt_quant(e));
    emit("    str = %s\n", str_con(e->name, e->nlen));
    emit("    strlen = %d\n", e->nlen);
    pge_gen_string(e, succ);
}


static void
pge_gen_backreference(PGE_Exp* e, const char* succ)
{
    char key[32];

    sprintf(key,"\"%d\"", e->group);
    trace("backref $%d %s", e->group, fmt_quant(e));
    emit("    classoffset $I0, match, \"PGE::Match\"\n");
    emit("    $I0 += 4\n");
    emit("    getattribute gr_cap, match, $I0\n");
    emit("    $I0 = defined gr_cap[%s]\n", key);
    emit("    unless $I0 goto %s\n", succ);
    emit("    $P0 = gr_cap[%s]\n", key);
    emit("    $I0 = $P0[-2]\n");
    emit("    $I1 = $P0[-1]\n");
    emit("    if $I0 >= $I1 goto %s\n", succ);
    emit("    strlen = $I1 - $I0\n");
    emit("    substr str, target, $I0, strlen\n");
    pge_gen_string(e, succ);
}


static void
pge_gen_concat(PGE_Exp* e, const char* succ)
{
    char succ2[20];
  
    emit("    #concat R%d, R%d\n", e->exp1->id, e->exp2->id); 
    sprintf(succ2,"R%d",e->exp2->id);
    pge_gen_exp(e->exp1, succ2);
    pge_gen_exp(e->exp2, succ);
}


/* XXX: add some docs that describes how this works! */
/* XXX: add check to prevent infinite recursion on zero-length match */
static void
pge_gen_group(PGE_Exp* e, const char* succ)
{
    char repsub[32];
    char r1sub[32];
    char key[32];
    char c1, c2;

    c1 = '['; c2 = ']';
    if (e->group >= 0) { c1 = '('; c2 = ')'; }
    sprintf(repsub, "R%d_repeat", e->id);
    sprintf(r1sub, "R%d", e->exp1->id);
    sprintf(key,"\"%d\"", e->group);

    trace("group %s %c %s %c %s", key, c1, r1sub, c2, fmt_quant(e));

    /* for unquantified, non-capturing groups, don't bother with the
       group code */
    if (e->min == 1 && e->max == 1 && e->group < 0) {
        pge_gen_exp(e->exp1, succ);
        return;
    }

    /* otherwise, we have work to do */

    /* GROUP: initialization
       This first part sets up the initial structures for a repeating group. 
       We need a repeat count and (possibly) a captures hash. */
    emit("    classoffset $I0, match, \"PGE::Match\"\n");
    emit("    $I0 += 3\n");
    emit("    getattribute gr_rep, match, $I0\n");
    emit("    $I1 = exists gr_rep[%s]\n", key);
    emit("    if $I1 goto R%d_1\n", e->id);
    emit("    new $P1, .PerlInt\n");
    emit("    gr_rep[%s] = $P1\n", key);
    emit("  R%d_1:\n", e->id);

    if (e->group >= 0) { 
        emit("    inc $I0\n");
        emit("    getattribute gr_cap, match, $I0\n");
        emit("    $I1 = exists gr_cap[%s]\n", key);
        emit("    if $I1 goto R%d_2\n", e->id);
        emit("    new $P1, .PerlArray\n");
        emit("    gr_cap[%s] = $P1\n", key);
        emit("  R%d_2:\n", e->id);
    }

    /* okay, make our first call to the subgroup.  We don't use
       emitsub() here because we have to capture cuts on the group. */
    emit("    $P1 = gr_rep[%s]\n", key);
    emit("    $I1 = $P1\n");
    emit("    $P1 = 0\n");
    /* emitsub(repsub, "pos", "gr_rep", "$I1", 0); */
    emit("    save pos\n");
    emit("    save gr_rep\n");
    emit("    save $I1\n");
    emit("    bsr %s\n", repsub);
    emit("    restore $I1\n");
    emit("    restore gr_rep\n");
    emit("    restore pos\n");
    emit("    $P1 = gr_rep[%s]\n", key);
    emit("    $P1 = $I1\n");
    emit("    goto fail\n\n");

    /* GROUP: repeat code
       This code is called whenever we reach the end of the group's
       subexpression.  It handles closing any outstanding capture, and 
       repeats the group if the quantifier requires it. */
    emit("  %s:\n", repsub);
    emit("    classoffset $I0, match, \"PGE::Match\"\n");
    emit("    $I0 += 3\n");
    emit("    getattribute $P0, match, $I0\n");
    emit("    gr_rep = $P0[%s]\n", key);
    if (e->group >= 0) { 
        emit("    inc $I0\n");
        emit("    getattribute $P0, match, $I0\n");
        emit("    gr_cap = $P0[%s]\n", key);
        emit("    if gr_rep < 1 goto %s_1\n", repsub);  /* save prev cap end */
        emit("    push gr_cap, pos\n");
    }

    emit("  %s_1:\n", repsub);
    if (e->isgreedy) {
        if (e->max != PGE_INF) 
            emit("    if gr_rep >= %d goto %s_2\n", e->max, repsub);
        emit("    inc gr_rep\n");
        if (e->group >= 0)
            emit("    push gr_cap, pos\n");         /* save next cap start */
        emitsub(r1sub, "pos", "gr_cap", "gr_rep", 0);
        if (e->group >= 0)
            emit("    $I0 = pop gr_cap\n");        /* remove next cap start */
        emit("    dec gr_rep\n");
        emit("  %s_2:\n", repsub);
        if (e->min > 0) 
            emit("    if gr_rep < %d goto %s_fail\n", e->min, repsub);
        emitsub(succ, "pos", "gr_cap", "gr_rep", 0);
    } 
    else { /* group lazy */
        if (e->min > 0)
            emit("    if gr_rep < %d goto %s_3\n", e->min, repsub);
        emitsub(succ, "pos", "gr_cap", "gr_rep", 0);
        emit("  %s_3:\n", repsub);
        if (e->max != PGE_INF)
            emit("    if gr_rep >= %d goto %s_fail\n", e->max, repsub);
        emit("    inc gr_rep\n");
        if (e->group >= 0) 
            emit("    push gr_cap, pos\n");         /* save next cap start */
        emitsub(r1sub, "pos", "gr_cap", "gr_rep", 0);
        if (e->group >= 0)
            emit("    $I0 = pop gr_cap\n");        /* remove next cap start */
        emit("    dec gr_rep\n");
    }  /* group lazy */

    emit("  %s_fail:\n", repsub);
    if (e->group >= 0) {
        emit("    if gr_rep < 1 goto fail\n", repsub);  
        emit("    $I0 = pop gr_cap\n");             /* remove prev cap end */
    }
    if (e->iscut) emit("    goto fail_group\n\n");
    else emit("    goto fail\n\n");

    pge_gen_exp(e->exp1, repsub);
}


static void
pge_gen_alt(PGE_Exp* e, const char* succ)
{
    char r1sub[32];

    trace("alt R%d | R%d", e->exp1->id, e->exp2->id);
  
    sprintf(r1sub, "R%d", e->exp1->id);
    emitsub(r1sub, "pos", 0);
    emit("    goto R%d\n\n", e->exp2->id);

    pge_gen_exp(e->exp1, succ);
    pge_gen_exp(e->exp2, succ);
}
    

static void
pge_gen_anchor(PGE_Exp* e, const char* succ)
{
    switch(e->type) {
    case PGE_ANCHOR_BOS:
        trace("^anchor");
        emit("    if pos != 0 goto fail\n");
        emit("    goto %s\n", succ);
        return;
    case PGE_ANCHOR_EOS:
        trace("anchor$");
        emit("    if pos != lastpos goto fail\n");
        emit("    goto %s\n", succ);
        return;
    case PGE_ANCHOR_BOL:
        trace("^^anchor");
        emit("    if pos == 0 goto %s\n", succ);
        emit("    if pos == lastpos goto fail\n");
        emit("    $I0 = pos - 1\n");
        emit("    substr $S0, target, $I0, 1\n");
        emit("    if $S0 == \"\\n\" goto %s\n", succ);
        emit("    goto fail\n\n");
        return;
    case PGE_ANCHOR_EOL:
        trace("anchor$$");
        emit("    if pos == lastpos goto R%d_1\n", e->id);
        emit("    substr $S0, target, pos, 1\n");
        emit("    if $S0 == \"\\n\" goto %s\n", succ);
        emit("    goto fail\n");
        emit("R%d_1:\n", e->id);
        emit("    $I0 = pos - 1\n");
        emit("    substr $S0, target, $I0, 1\n");
        emit("    if $S0 != \"\\n\" goto %s\n", succ);
        emit("    goto fail\n\n");
        return;
    default: break;
    }
}


static void
pge_gen_cut(PGE_Exp* e, const char* succ)
{
    if (e->type == PGE_CUT_ALT) {
        trace("::cut alt");
        emitsub(succ, 0);
        emit("    goto fail_group\n");
    }
    if (e->type == PGE_CUT_RULE) {
        trace("::cut rule");
        emit("    .yield(-2)\n");
        emit("    goto fail\n");
    }
}


static void 
pge_gen_exp(PGE_Exp* e, const char* succ)
{
    emitlcount();
    emit("  R%d:\n", e->id);
    switch (e->type) {
    case PGE_NULL_PATTERN: emit("    goto %s\n", e->id, succ); break;
    case PGE_PATTERN_END: pge_gen_pattern_end(e, succ); break;
    case PGE_DOT: pge_gen_dot(e, succ); break;
    case PGE_LITERAL: pge_gen_literal(e, succ); break;
    case PGE_CONCAT: pge_gen_concat(e, succ); break;
    case PGE_GROUP: pge_gen_group(e, succ); break;
    case PGE_ALT: pge_gen_alt(e, succ); break;
    case PGE_ANCHOR_BOS:
    case PGE_ANCHOR_EOS: 
    case PGE_ANCHOR_BOL:
    case PGE_ANCHOR_EOL: pge_gen_anchor(e, succ); break;
    case PGE_CUT_ALT: 
    case PGE_CUT_RULE: pge_gen_cut(e, succ); break;
    case PGE_BACKREFERENCE: pge_gen_backreference(e, succ); break;
    }
}


char* 
pge_gen(PGE_Exp* e)
{
    char r1sub[32];
    pge_cbuf_len = 0;
    pge_cbuf_lcount = 0;

    sprintf(r1sub, "R%d", e->id);

    if (pge_istraced) {
        emit(".macro trace(POS, LABEL)\n");
        emit("    $S31 = repeat ' ', .POS\n");
        emit("    print $S31\n");
        emit("    print .LABEL\n");
        emit("    print \"\\n\"\n");
        emit(".endm\n\n");
    }

    emit(".sub _PGE_Rule\n");
    emit("    .param string target\n");
    emit("    .local pmc match\n");
    emit("    .local pmc rulecor\n");
    emit("    .local pmc newmeth\n");
    emit("    newsub rulecor, .Coroutine, _PGE_Rule_cor\n");
    emit("    find_global newmeth, \"PGE::Match\", \"new\"\n");
    emit("    match = newmeth(target, rulecor)\n");
    emit("    match.\"_next\"()\n");
    emit("    .return(match)\n");
    emit(".end\n\n");

    emit(".sub _PGE_Rule_cor\n");
    emit("    .param pmc match\n");
    emit("    .param string target\n");
    emit("    .param int pos\n");
    emit("    .param int lastpos\n");
    emit("    .local int rep\n");
    emit("    .local int maxrep\n");
    emit("    .local pmc gr_rep\n");
    emit("    .local pmc gr_cap\n");
    emit("    .local int cutgrp\n");
    emit("    .local string str\n");
    emit("    .local int strlen\n");
    emit("    if pos >= 0 goto try_once_at_pos\n");
    emit("    pos = 0\n");
    if (!pge_is_bos_anchored(e)) {
        emit("  try_match:\n");
        emit("    if pos > lastpos goto fail_forever\n");
        emitsub(r1sub, "pos", 0);
        emit("    inc pos\n");
        emit("    goto try_match\n");
    }
    emit("  try_once_at_pos:\n");
    emitsub(r1sub, 0);
    emit("  fail_forever:\n");
    emit("    .yield(-2)\n");
    emit("    goto fail_forever\n\n");

    pge_gen_exp(e, 0);
    emit("  fail_group:\n");
    trace("fail_group");
    emit("    cutgrp = 1\n    ret\n");
    emit("  fail:\n");
    trace("fail");
    emit("    cutgrp = 0\n");
    emit("    ret\n");
    emit(".end\n");

    return pge_cbuf;
}

/*

item C<void pge_set_trace(int istraced)>

Used to turn on/off the .trace macros in the PIR code.  When enabled,
causes the regular expression output to be traced to standard output.
When disabled, the .trace macro becomes null (thus there's no overhead).

*/

void
pge_set_trace(int istraced)
{
    pge_istraced = istraced;
}

/*

=item C<void Parrot_lib_pge_init(Parrot_Interp interpreter, PMC* lib)>

Used when this module is loaded dynamically by Parrot's loadlib
instruction -- automatically initializes the pge engine.

=cut

*/

void 
Parrot_lib_pge_init(Parrot_Interp interpreter, PMC* lib)
{
    pge_init();
}

/*

=back

=head1 SEE ALSO

F<pge/pge.h> and F<pge/pge_parse.c>

=head1 HISTORY

Initial version by Patrick R. Michaud on 2004.11.16

=cut

*/

/*
 * Local variables:
 * c-indentation-style: bsd
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 * vim: expandtab shiftwidth=4:
 */
