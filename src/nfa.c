/*  Copyright (C) 2006, Index Data ApS
 *  See the file LICENSE for details.
 * 
 *  $Id: nfa.c,v 1.3 2006-05-03 11:36:14 mike Exp $ 
 *
 *  This is a simple NFA-based system for character set normalizing
 *  in yaz and zebra. Unlike standard NFA's, this operates on ranges of
 *  characters at a time, so as to keep the size small.
 *
 *  All characters are internally handled as unsigned 32-bit ints, so 
 *  this works well with unicode. Translating to and from utf-8 is trivial, if
 *  need be.
 *
 *  The NFA stores a translation-thing in the terminal nodes. It does not
 *  concern itself with what that thing is, for us it is juts a void*
 *
 *  The module consists of two parts: Building the NFA, and translating
 *  strings with it.
 */

#include <stdio.h>
#include <yaz/nfa.h>
#include <yaz/nmem.h> 

/* * * * * * * * * *
 * Data structures
 * * * * * * * * * */

typedef struct yaz_nfa_backref_info yaz_backref_info;

struct yaz_nfa_backref_info {
    yaz_nfa_char *start;
    yaz_nfa_char *end;
};

struct yaz_nfa {
    NMEM nmem; 
    int nstates;   /* how many states do we have */
    int nbackrefs;  /* how many backrefs do we have */
    yaz_nfa_state *laststate; /* points to the last in the circular list */
    yaz_nfa_state *firststate; /* the initial state */
    yaz_backref_info *curr_backrefs; /* the backrefs we are matching */
    yaz_backref_info *best_backrefs; /* backrefs of the best match so far*/
};

struct yaz_nfa_state {
    int num; /* state number. used for resoving ambiguities, and for dumping */
    void *result; /* signals a terminal state, and gives the result */
    yaz_nfa_transition *lasttrans; /* circular list of transitions */
    yaz_nfa_state *next; /* Circular linked list */
    int backref_start; /* which backreference starts here. 0 if none */
    int backref_end; /* which backreference ends here. 0 if none */
} ;

struct yaz_nfa_transition {
      yaz_nfa_state *to_state;  /* where to */
      yaz_nfa_char range_start; 
      yaz_nfa_char range_end;
      yaz_nfa_transition *next; /* a linked list of them */
} ;

/* a range from 1 down to 0 is impossible, and is used to denote an */
/* empty (epsilon) transition */
#define EMPTY_START 1
#define EMPTY_END   0

/* a limit for the number of empty transitions we can have in a row before */
/* we declare this a loop */
#define LOOP_LIMIT 100




/* * * * * * *
 * Initializing and destroying whole nfa's
 * * * * * * */

yaz_nfa *yaz_nfa_init() {
    NMEM my_nmem = nmem_create();
    yaz_nfa *n=nmem_malloc(my_nmem, sizeof(yaz_nfa));
    n->nmem=my_nmem;
    n->nstates=0;
    n->laststate=0;
    n->firststate=0;
    n->nbackrefs=0;
    n->curr_backrefs=0;
    n->best_backrefs=0;
    return n;
}

void yaz_nfa_destroy(yaz_nfa *n) {
    nmem_destroy(n->nmem);
}


/* * * * * *
 * Low-level interface to building the NFA
 * * * * * */

yaz_nfa_state *yaz_nfa_add_state(yaz_nfa *n) {
    yaz_nfa_state *s = nmem_malloc(n->nmem,sizeof(yaz_nfa_state));
    s->num = (n->nstates)++;
    s->result=0;
    s->lasttrans=0;
    s->backref_start=0;
    s->backref_end=0;
    if (n->laststate) { 
        s->next=n->laststate->next;
        n->laststate->next=s;
        n->laststate=s;
    } else { /* first state */
        n->laststate=s;
        n->firststate=s;
        s->next=s;
    }
    return s;
}

int yaz_nfa_set_result(yaz_nfa *n, yaz_nfa_state *s,void *result) {
    if ((s->result)&&result)
        return 1;
    s->result=result;
    return 0;
}

void *yaz_nfa_get_result(yaz_nfa *n, yaz_nfa_state *s) {
    if (!s)
        return 0;
    return s->result;
}

int yaz_nfa_set_backref(yaz_nfa *n, yaz_nfa_state *s,
           int backref_number,
           int is_start ){
    if (is_start) {
        if (s->backref_start)
            return 1;
        s->backref_start=backref_number;
        if (n->nbackrefs<backref_number) {
            n->nbackrefs=backref_number;
            n->curr_backrefs=0;
            n->best_backrefs=0;
            /* clear them just in case we have already matched on */
            /* with this nfa, and created a too small backref table */
            /* we will reallocate when matching. */
        }
    } else {
        if (s->backref_end)
            return 1;
        if (n->nbackrefs<backref_number) 
            return 2; /* can't end a backref that has not been started */
        s->backref_end=backref_number;
    }
    return 0; /* ok */
}

int yaz_nfa_get_backref(yaz_nfa *n, yaz_nfa_state *s,
                int is_start ) {
    if (!s) 
        return 0;
    if (is_start)
        return s->backref_start;
    else
        return s->backref_end;
}

void yaz_nfa_add_transition(yaz_nfa *n, 
        yaz_nfa_state *from_state, 
        yaz_nfa_state *to_state,
        yaz_nfa_char range_start, 
        yaz_nfa_char range_end) {
    yaz_nfa_transition *t=nmem_malloc(n->nmem,sizeof(yaz_nfa_transition));
    t->range_start=range_start;
    t->range_end=range_end;
    t->to_state=to_state;
    if (from_state->lasttrans) {
        t->next= from_state->lasttrans->next;
        from_state->lasttrans->next=t;
        from_state->lasttrans=t;
    } else { /* first trans */
        from_state->lasttrans=t;
        t->next=t;
    }
}

void yaz_nfa_add_empty_transition( yaz_nfa *n,
        yaz_nfa_state *from_state,
        yaz_nfa_state *to_state) {
    yaz_nfa_add_transition(n,from_state,to_state,
              EMPTY_START,EMPTY_END);
}

/* * * * * * * *
 * Medium-level interface
 * * * * * * * */

/* Finds a transition from s where the range is exactly c..c */
/* There should only be one such transition */
static yaz_nfa_state *find_single_trans(
          yaz_nfa_state *s, 
          yaz_nfa_char range_start,
          yaz_nfa_char range_end) {
    yaz_nfa_transition *t=s->lasttrans;
    if (!t)
        return 0;
    do {
        t=t->next;
        if ( ( t->range_start == range_start ) && ( t->range_end == range_end) )
            return t->to_state;
    } while (t != s->lasttrans );
    return 0;
}


yaz_nfa_state *yaz_nfa_add_range(yaz_nfa *n, 
        yaz_nfa_state *s,
        yaz_nfa_char range_start, 
        yaz_nfa_char range_end) {
    yaz_nfa_state *nextstate;
    if (!s) /* default to top-level of the nfa */
        s=n->firststate;
    nextstate=find_single_trans(s,range_start,range_end);
    if (!nextstate) {
        nextstate = yaz_nfa_add_state(n);
        yaz_nfa_add_transition(n, s, nextstate, range_start, range_end);
    }
    return nextstate;
}

yaz_nfa_state *yaz_nfa_add_sequence(yaz_nfa *n, 
        yaz_nfa_state *s, 
        yaz_nfa_char *seq ){
    yaz_nfa_state *nextstate;
    if (!s) /* default to top-level of the nfa */
        s=n->firststate;
    nextstate=find_single_trans(s,*seq,*seq);
    if (nextstate) {
        seq++;
        if (!*seq) /* whole sequence matched */
            return nextstate;
        else
            return yaz_nfa_add_sequence(n, nextstate, seq);
    } else { /* no next state, build the rest */
        while (*seq) {
            s=yaz_nfa_add_range(n,s, *seq, *seq);
            seq++;
        }
        return s;
    }
    return 0; /* compiler shut up, we return somewhere above */
}



/* * * * * * *
 * Searching the NFA 
 * * * * * * */

struct matcher {
    yaz_nfa *n; 
    yaz_nfa_char *longest;
    int bestnode;
    void *result;
    int errorcode;
    int empties; /* count how many consecutive empty transitions */
};

/* Finds the longest match. In case of ties, chooses the one with the 
 * lowest numbered state. Keep track of the back references. Recursively
 * traverses the NFA. Keeps track of the best hit it has found. */

static void match_state(
              yaz_nfa_state *s, 
              yaz_nfa_char *inchar,
              size_t incharsleft,
              struct matcher *m ) {
    yaz_nfa_transition *t=s->lasttrans;
    if (s->backref_start) 
        m->n->curr_backrefs[s->backref_start].start=inchar;
    if (s->backref_end) 
        m->n->curr_backrefs[s->backref_end].end=inchar;
    if (t) {
        if (incharsleft) {
            do {
                t=t->next;
                if ( (( t->range_start <= *inchar ) && ( t->range_end >= *inchar )) ){
                    m->empties=0;
                    match_state(t->to_state, inchar+1,incharsleft-1,m);
                    /* yes, descent to all matching nodes, even if overrun, */
                    /* since we can find a better one later */
                } else if (( t->range_start==EMPTY_START) && (t->range_end==EMPTY_END)) {
                    if ( m->empties++ > LOOP_LIMIT ) 
                        m->errorcode= YAZ_NFA_LOOP;
                    else
                        match_state(t->to_state, inchar,incharsleft,m);
                }
            } while (t != s->lasttrans );
        } else {
            m->errorcode=YAZ_NFA_OVERRUN;
        }
    }
    if (s->result) { /* terminal node */
        if ( (m->longest < inchar) ||  /* longer result */
             ((m->longest == inchar)&&(m->bestnode<s->num)) ){
              /* or as long, but with lower node number. Still better */
           int i;
           m->longest=inchar;
           m->bestnode=s->num;
           m->result=s->result;
           if (m->n->curr_backrefs) 
               for (i=0; i<m->n->nbackrefs;i++) 
                   m->n->best_backrefs[i]=m->n->curr_backrefs[i];
        }
    }
    if (s->backref_start) 
        m->n->curr_backrefs[s->backref_start].start=0;
    if (s->backref_end) 
        m->n->curr_backrefs[s->backref_end].end=0;
} /* match_state */

int yaz_nfa_match(yaz_nfa *n, 
           yaz_nfa_char **inbuff, 
           size_t incharsleft,
           void **result ){
    struct matcher m;
    if (!n->firststate)
        return YAZ_NFA_NOMATCH; 
    m.n=n;
    m.longest=*inbuff;
    m.bestnode=n->nstates;
    m.result=0;
    m.errorcode=0;
    if ((!n->curr_backrefs) && (n->nbackrefs)){
        int sz=sizeof( struct yaz_nfa_backref_info) * (n->nbackrefs+1);
        n->curr_backrefs=nmem_malloc( n->nmem, sz);
        n->best_backrefs=nmem_malloc( n->nmem, sz);
    }
    

    match_state(n->firststate, *inbuff, incharsleft, &m);
    if (m.result) {
        *result=m.result;
        *inbuff=m.longest;
        if (m.errorcode)
            return m.errorcode;
        else
            return YAZ_NFA_SUCCESS;
    }
    return YAZ_NFA_NOMATCH;
}




/* * * * * * * * *
 * Debug routines
 * * * * * * */

yaz_nfa_state *yaz_nfa_get_first(yaz_nfa *n){
    if (!n)
        return 0;
    return n->firststate;
}

yaz_nfa_state *yaz_nfa_get_next(yaz_nfa *n, yaz_nfa_state *s){
    if (n && s) {
        if (s==n->laststate)
            return 0;
        return s->next;
    }
    return 0;
}


static void dump_trans(FILE *F, yaz_nfa_transition *t ) {
    char c1;
    char c2;
    char *e;
    c1=t->range_start;
    c2=t->range_end;
    e="";
    if ( (t->range_start <= ' ') || (t->range_start>'z'))
        c1='?';
    if ( (t->range_end <= ' ') || (t->range_end>'z'))
        c2='?';
    if ((t->range_start==EMPTY_START) && (t->range_end==EMPTY_END)) {
        e="(empty)";
    }
    fprintf(F,"    -> [%d]  %s '%c' %x - '%c' %x \n",
            t->to_state->num, e,
            c1, t->range_start,
            c2, t->range_end );
}

static void dump_state(FILE *F, yaz_nfa_state *s,
            char *(*strfunc)(void *) ) {
    yaz_nfa_transition *t;
    char *resultstring="";
    if (s->result) {
        if (strfunc)  {
            resultstring=(*strfunc)(s->result);
        }
        else
            resultstring=s->result;
    }
    fprintf(F,"  state [%d] %s %s",
            s->num, s->result?"(FINAL)":"",resultstring );
    if (s->backref_start) {
        fprintf(F," start-%d",s->backref_start);
    }
    if (s->backref_end) {
        fprintf(F," end-%d",s->backref_end);
    }
    fprintf(F,"\n");
    t=s->lasttrans;
    if (!t) {
        fprintf(F,"    (no transitions)\n");
    } else {
        do {
            t=t->next;
            dump_trans(F,t);
        } while (t != s->lasttrans);
    }

}

void yaz_nfa_dump(FILE *F, yaz_nfa *n,
            char *(*strfunc)(void *) ) {
    yaz_nfa_state *s;
    if (!F)   /* lazy programmers can just pass 0 for F */
        F=stdout;
    fprintf(F,"The NFA has %d states \n",n->nstates);
    s=n->laststate;
    if (s) {
        do {
            s=s->next;
            dump_state(F,s, strfunc);
        } while (s != n->laststate);
    }
}



/* 
 * Local variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */