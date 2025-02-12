/* Filtering of objects based on simple expressions.
 * This powers the FILTER option of Vector Sets, but it is otherwise
 * general code to be used when we want to tell if a given object (with fields)
 * passes or fails a given test for scalars, strings, ...
 *
 * Copyright(C) 2024-2025 Salvatore Sanfilippo. All Rights Reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define EXPR_TOKEN_EOF 0
#define EXPR_TOKEN_NUM 1
#define EXPR_TOKEN_STR 2
#define EXPR_TOKEN_TUPLE 3
#define EXPR_TOKEN_SELECTOR 4
#define EXPR_TOKEN_OP 5

#define EXPR_OP_OPAREN 0  /* ( */
#define EXPR_OP_CPAREN 1  /* ) */
#define EXPR_OP_NOT    2  /* ! */
#define EXPR_OP_POW    3  /* ** */
#define EXPR_OP_MULT   4  /* * */
#define EXPR_OP_DIV    5  /* / */
#define EXPR_OP_MOD    6  /* % */
#define EXPR_OP_SUM    7  /* + */
#define EXPR_OP_DIFF   8  /* - */
#define EXPR_OP_GT     9  /* > */
#define EXPR_OP_GTE    10 /* >= */
#define EXPR_OP_LT     11 /* < */
#define EXPR_OP_LTE    12 /* <= */
#define EXPR_OP_EQ     13 /* == */
#define EXPR_OP_NEQ    14 /* != */
#define EXPR_OP_IN     15 /* in */
#define EXPR_OP_AND    16 /* and */
#define EXPR_OP_OR     17 /* or */

/* This structure represents a token in our expression. It's either
 * literals like 4, "foo", or operators like "+", "-", "and", or
 * json selectors, that start with a dot: ".age", ".properties.somearray[1]" */
typedef struct exprtoken {
    int token_type;         // Token type of the just parsed token.
    union {
        double num;         // Value for EXPR_TOKEN_NUM.
        struct {
            char *start;    // String pointer for EXPR_TOKEN_STR / SELECTOR.
            size_t len;     // String len for EXPR_TOKEN_STR / SELECTOR.
        } str;
        int opcode;         // Opcode ID for EXPR_TOKEN_OP.
        struct {
            struct exprtoken *ele;
            size_t len;
        } tuple;            // Tuples are like [1, 2, 3] for "in" operator.
    };
} exprtoken;

/* Simple stack of expr tokens. This is used both to represent the stack
 * of values and the stack of operands during VM execution. */
typedef struct exprstack {
    exprtoken **items;
    int numitems;
    int allocsize;
} exprstack;

typedef struct exprstate {
    char *expr;             /* Expression string to compile. Note that
                             * expression token strings point directly to this
                             * string. */
    char *p;                // Currnet position inside 'expr', while parsing.
    exprtoken current;      // Last token parsed.
    int syntax_error;       // True if a syntax error was found compiling.

    // Virtual machine state.
    exprstack values_stack;
    exprstack ops_stack;
    exprstack program;      // Expression "compiled" into a sequence of tokens.
    int ip;                 // Instruction pointer inside "program".
} exprstate;

/* Valid operators. */
struct {
    char *opname;
    int oplen;
    int opcode;
    int precedence;
} ExprOptable[] = {
    {"(",   1,  EXPR_OP_OPAREN,  7},
    {")",   1,  EXPR_OP_CPAREN,  7},
    {"!",   1,  EXPR_OP_NOT,     6},
    {"**",  2,  EXPR_OP_POW,     5},
    {"*",   1,  EXPR_OP_MULT,    4},
    {"/",   1,  EXPR_OP_DIV,     4},
    {"%",   1,  EXPR_OP_MOD,     4},
    {"+",   1,  EXPR_OP_SUM,     3},
    {"-",   1,  EXPR_OP_DIFF,    3},
    {">",   1,  EXPR_OP_GT,      2},
    {">=",  2,  EXPR_OP_GTE,     2},
    {"<",   1,  EXPR_OP_LT,      2},
    {"<=",  2,  EXPR_OP_LTE,     2},
    {"==",  2,  EXPR_OP_EQ,      2},
    {"!=",  2,  EXPR_OP_NEQ,     2},
    {"in",  2,  EXPR_OP_IN,      2},
    {"and", 3,  EXPR_OP_AND,     1},
    {"or",  2,  EXPR_OP_OR,      0},
    {NULL,  0,  0,               0}   // Terminator.
};

/* ============================== Stack handling ============================ */

#include <stdlib.h>
#include <string.h>

#define EXPR_STACK_INITIAL_SIZE 16

/* Initialize a new expression stack. */
void exprStackInit(exprstack *stack) {
    stack->items = malloc(sizeof(exprtoken*) * EXPR_STACK_INITIAL_SIZE);
    stack->numitems = 0;
    stack->allocsize = EXPR_STACK_INITIAL_SIZE;
}

/* Push a token pointer onto the stack. Return 0 on out of memory
 * (leaving the stack as it is), 1 on success. */
int exprStackPush(exprstack *stack, exprtoken *token) {
    /* Check if we need to grow the stack. */
    if (stack->numitems == stack->allocsize) {
        size_t newsize = stack->allocsize * 2;
        exprtoken **newitems =
            realloc(stack->items, sizeof(exprtoken*) * newsize);
        if (newitems == NULL) return 0;
        stack->items = newitems;
        stack->allocsize = newsize;
    }
    stack->items[stack->numitems] = token;
    stack->numitems++;
    return 1;
}

/* Pop a token pointer from the stack. Return NULL if the stack is
 * empty. */
exprtoken *exprStackPop(exprstack *stack) {
    if (stack->numitems == 0) return NULL;
    stack->numitems--;
    return stack->items[stack->numitems];
}

/* Just return the last element pushed, without consuming it. */
exprtoken *exprStackPeek(exprstack *stack) {
    if (stack->numitems == 0) return NULL;
    return stack->items[stack->numitems-1];
}

/* Free the stack structure state, including the items it contains, that are
 * assumed to be heap allocated. The passed pointer itself is not freed. */
void exprStackFree(exprstack *stack) {
    for (int j = 0; j < stack->numitems; j++)
        free(stack->items[j]);
    free(stack->items);
}

/* =========================== Expression compilation ======================= */

void exprConsumeSpaces(exprstate *es) {
    while(es->p[0] && isspace(es->p[0])) es->p++;
}

#define EXPR_OP_SPECIALCHARS "+-*%/!()<>="
void exprParseOperatorOrSelector(exprstate *es) {
    es->current.token_type = es->p[0] == '.' ? EXPR_TOKEN_SELECTOR : EXPR_TOKEN_OP;
    char *start = es->p;
    while(es->p[0] &&
          (isalpha(es->p[0]) ||
           strchr(EXPR_OP_SPECIALCHARS,es->p[0]) != NULL))
    {
        es->p++;
    }
    size_t matchlen = es->p - start;

    /* If this is not a selector for an attribute to retrive, then
     * it must be one of the valid operators. */
    size_t bestlen = 0;
    if (es->current.token_type == EXPR_TOKEN_OP) {
        int j;
        printf("maxlen: %d\n", (int) matchlen);
        for (j = 0; ExprOptable[j].opname != NULL; j++) {
            if (ExprOptable[j].oplen > matchlen) continue;
            if (memcmp(ExprOptable[j].opname, start, ExprOptable[j].oplen) != 0)
            {
                continue;
            }
            printf("%s\n", ExprOptable[j].opname);
            if (ExprOptable[j].oplen > bestlen) {
                es->current.opcode = ExprOptable[j].opcode;
                bestlen = ExprOptable[j].oplen;
            }
        }
        if (bestlen == 0) {
            printf("HERE %s len:%d\n", start, (int)matchlen);
            es->syntax_error++;
        } else {
            es->p = start + bestlen;
        }
    } else {
        es->current.str.start = start;
        es->current.str.len = matchlen;
    }
}

void exprParseNumber(exprstate *es) {
    es->current.token_type = EXPR_TOKEN_NUM;
    char num[64];
    int idx = 0;
    while(isdigit(es->p[0]) || (idx == 0 && es->p[0] == '-')) {
        if (idx == sizeof(num)-1) {
            es->syntax_error++; // Number is too long.
            break;
        }
        num[idx++] = es->p[0];
        es->p++;
    }
    num[idx] = 0;

    char *endptr;
    es->current.num = strtod(num, &endptr);
    if (*endptr != '\0') es->syntax_error++;
}

void exprParseString(exprstate *es) {
    char quote = es->p[0];  /* Store the quote type (' or "). */
    es->p++;                /* Skip opening quote. */

    es->current.token_type = EXPR_TOKEN_STR;
    es->current.str.start = es->p;

    while(es->p[0] != '\0') {
        if (es->p[0] == '\\' && es->p[1] != '\0') {
            es->p += 2; // Skip escaped char.
            continue;
        }
        if (es->p[0] == quote) {
            es->current.str.len =
                es->p - es->current.str.start;
            es->p++; // Skip closing quote.
            return;
        }
        es->p++;
    }
    /* If we reach here, string was not terminated. */
    es->syntax_error++;
}

/* Deallocate the object returned by exprCompile(). */
void exprFree(exprstate *es) {
    if (es == NULL) return;

    /* Free the original expression string. */
    if (es->expr) free(es->expr);

    /* Free all stacks. */
    exprStackFree(&es->values_stack);
    exprStackFree(&es->ops_stack);
    exprStackFree(&es->program);

    /* Free the state object itself. */
    free(es);
}

/* Compile the provided expression into a stack of tokens within
 * context that can be used to execute it. */
exprstate *exprCompile(char *expr, char **errptr) {
    /* Initialize expression state */
    exprstate *es = malloc(sizeof(exprstate));
    if (!es) return NULL;

    es->expr = strdup(expr);
    if (!es->expr) return NULL;
    es->p = es->expr;
    es->syntax_error = 0;

    /* Initialize all stacks */
    exprStackInit(&es->values_stack);
    exprStackInit(&es->ops_stack);
    exprStackInit(&es->program);
    es->ip = 0;

    /* Main parsing loop. */
    while(1) {
        exprConsumeSpaces(es);

        /* Set a flag to see if we can consider the - part of the
         * number, or an operator. */
        int minus_is_number = 0; // By default is an operator.

        exprtoken *last = exprStackPeek(&es->program);
        if (last == NULL) {
            /* If we are at the start of an expression, the minus is
             * considered a number. */
            minus_is_number = 1;
        } else if (last->token_type == EXPR_TOKEN_OP &&
                   last->opcode != EXPR_OP_CPAREN)
        {
            /* Also, if the previous token was an operator, the minus
             * is considered a number, unless the previous operator is
             * a closing parens. In such case it's like (...) -5, or alike
             * and we want to emit an operator. */
            minus_is_number = 1;
        }

        /* Parse based on the current character. */
        printf("Compiling... (minus_number:%d) %s\n", minus_is_number, es->p);
        if (*es->p == '\0') {
            es->current.token_type = EXPR_TOKEN_EOF;
        } else if (isdigit(*es->p) ||
                  (minus_is_number && *es->p == '-' && isdigit(es->p[1])))
        {
            exprParseNumber(es);
        } else if (*es->p == '"' || *es->p == '\'') {
            exprParseString(es);
        } else if (*es->p == '.' || isalpha(*es->p) ||
                  strchr(EXPR_OP_SPECIALCHARS, *es->p)) {
            exprParseOperatorOrSelector(es);
        } else {
            es->syntax_error++;
        }

        if (es->syntax_error) {
            if (errptr) *errptr = expr + (es->p - es->expr);
            goto error;
        }

        /* Allocate and copy current token to program stack */
        exprtoken *token = malloc(sizeof(exprtoken));
        if (!token) goto error;

        *token = es->current;  /* Copy the entire structure */

        printf("Pushing %d\n", es->current.token_type);
        if (!exprStackPush(&es->program, token)) {
            free(token);
            goto error;
        }
        if (es->current.token_type == EXPR_TOKEN_EOF) break;
    }
    return es;

error:
    exprFree(es);
    return NULL;
}

/* ============================ Simple test main ============================ */

#ifdef TEST_MAIN
void exprPrintToken(exprtoken *t) {
    switch(t->token_type) {
        case EXPR_TOKEN_EOF:
            printf("EOF");
            break;
        case EXPR_TOKEN_NUM:
            printf("NUM:%g", t->num);
            break;
        case EXPR_TOKEN_STR:
            printf("STR:\"%.*s\"", (int)t->str.len, t->str.start);
            break;
        case EXPR_TOKEN_SELECTOR:
            printf("SEL:%.*s", (int)t->str.len, t->str.start);
            break;
        case EXPR_TOKEN_OP:
            printf("OP:");
            for (int i = 0; ExprOptable[i].opname != NULL; i++) {
                if (ExprOptable[i].opcode == t->opcode) {
                    printf("%s", ExprOptable[i].opname);
                    break;
                }
            }
            break;
        default:
            printf("UNKNOWN");
            break;
    }
}

void exprPrintStack(exprstack *stack, const char *name) {
    printf("%s (%d items):", name, stack->numitems);
    for (int j = 0; j < stack->numitems; j++) {
        printf(" ");
        exprPrintToken(stack->items[j]);
    }
    printf("\n");
}

int main(int argc, char **argv) {

    char *testexpr = "(5*2)-3 and 'foo'";
    if (argc >= 1) testexpr = argv[1];

    printf("Compiling expression: %s\n", testexpr);

    char *errptr = NULL;
    exprstate *es = exprCompile(testexpr,&errptr);
    if (es == NULL) {
        printf("Compilation failed near \"...%s\"\n", errptr);
        return 1;
    }

    if (es->syntax_error) {
        printf("Syntax error found\n");
        return 1;
    }

    exprPrintStack(&es->program, "Program");
    exprFree(es);
    return 0;
}
#endif
