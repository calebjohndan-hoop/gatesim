/*
 * GateSim: a tiny gate-level digital logic simulator.
 *
 * Usage:
 *     ./gatesim circuit.circ                      full truth table
 *     ./gatesim circuit.circ A=1 B=0 CIN=1        one test vector
 *     ./gatesim circuit.circ --stats              add a circuit report
 *
 * File format (one statement per line, '#' starts a comment):
 *
 *     INPUT  A B
 *     OUTPUT SUM CARRY
 *     XOR    SUM   A B          # GATE  output  input1 input2 ...
 *     AND    CARRY A B
 *
 * Reusable modules:
 *
 *     MODULE HA A B -> S C      # name, inputs, '->', outputs
 *         XOR S A B
 *         AND C A B
 *     END
 *     INSTANCE HA U1 X Y -> SUM1 CARRY1
 *
 * Supported gates: AND OR XOR NAND NOR XNOR (2 or more inputs),
 *                  NOT BUF (exactly 1 input).
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_NAME        32   /* longest wire name, including '\0'        */
#define MAX_WIRES       512  /* total wires in a circuit                 */
#define MAX_GATES       512  /* total gates in a circuit                 */
#define MAX_GATE_INPUTS 8    /* fan-in limit per gate                    */
#define MAX_INPUTS      16   /* 2^16 = 65536 truth table rows            */
#define MAX_LINE        512  /* longest line we read from the file       */
#define MAX_MODULES     16
#define MAX_MOD_LINES   64   /* gates per module body                    */
#define MAX_PORTS       16   /* inputs or outputs per module             */
#define DELIMS          " \t\r\n,"

typedef enum { G_AND, G_OR, G_NOT, G_XOR, G_NAND, G_NOR, G_XNOR, G_BUF, G_COUNT } GateType;

static const char *GATE_NAMES[G_COUNT] =
{
    "AND", "OR", "NOT", "XOR", "NAND", "NOR", "XNOR", "BUF"
};

/* A wire is a named signal. It is either a circuit INPUT, or it is
 * driven by exactly one gate (driver = index of that gate). */
typedef struct
{
    char name[MAX_NAME];
    int  value;     /* current logic value: 0 or 1                   */
    int  driver;    /* index into gates[], or -1 if nothing drives it */
    int  is_input;
    int  is_output;
} Wire;

typedef struct
{
    GateType type;
    int out;                    /* index into wires[]   */
    int in[MAX_GATE_INPUTS];    /* indices into wires[] */
    int n_in;
    int line;                   /* source line, for error messages */
} Gate;

/* A module is a reusable sub-circuit. We store its body as text and
 * copy it out (with renamed wires) every time it is instantiated. */
typedef struct
{
    char name[MAX_NAME];
    char in[MAX_PORTS][MAX_NAME];
    int  n_in;
    char out[MAX_PORTS][MAX_NAME];
    int  n_out;
    char body[MAX_MOD_LINES][MAX_LINE];
    int  n_body;
} Module;

/* The whole circuit lives in these arrays. Indices act like pointers
 * but are easier to print and debug than real pointers. */
static Wire   wires[MAX_WIRES];
static int    n_wires = 0;
static Gate   gates[MAX_GATES];
static int    n_gates = 0;
static int    inputs[MAX_INPUTS];
static int    n_inputs = 0;
static int    outputs[MAX_WIRES];
static int    n_outputs = 0;
static int    order[MAX_GATES];   /* gate indices in evaluation order */
static Module modules[MAX_MODULES];
static int    n_modules = 0;

/* ---------- small helpers ---------- */

/* Case-insensitive string compare, so "and", "AND" and "And" all work. */
static int same_word(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (toupper((unsigned char) *a) != toupper((unsigned char) *b))
        {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int valid_name(const char *s)
{
    if (strlen(s) >= MAX_NAME)
    {
        return 0;
    }
    if (!isalpha((unsigned char) s[0]) && s[0] != '_')
    {
        return 0;
    }
    for (int i = 1; s[i]; i++)
    {
        if (!isalnum((unsigned char) s[i]) && s[i] != '_')
        {
            return 0;
        }
    }
    return 1;
}

static int parse_gate_type(const char *s, GateType *t)
{
    for (int i = 0; i < G_COUNT; i++)
    {
        if (same_word(s, GATE_NAMES[i]))
        {
            *t = (GateType) i;
            return 1;
        }
    }
    return 0;
}

static int find_wire(const char *name)
{
    for (int i = 0; i < n_wires; i++)
    {
        if (strcmp(wires[i].name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

/* Return the index of wire `name`, creating it if needed.
 * `check` = 1 for names typed by the user (must be valid identifiers).
 * `check` = 0 for names GateSim generates itself, like "U0.T1", whose
 * dot can never clash with a user name. Returns -1 on error. */
static int find_or_add_wire(const char *name, int line, int check)
{
    if (strlen(name) >= MAX_NAME || (check && !valid_name(name)))
    {
        fprintf(stderr, "line %d: invalid wire name '%s'\n", line, name);
        return -1;
    }
    int i = find_wire(name);
    if (i >= 0)
    {
        return i;
    }
    if (n_wires >= MAX_WIRES)
    {
        fprintf(stderr, "line %d: too many wires (max %d)\n", line, MAX_WIRES);
        return -1;
    }
    Wire *w = &wires[n_wires];
    strcpy(w->name, name);
    w->value = 0;
    w->driver = -1;
    w->is_input = 0;
    w->is_output = 0;
    return n_wires++;
}

static int arity_ok(GateType t, int n_in)
{
    int single = (t == G_NOT || t == G_BUF);
    return single ? n_in == 1 : n_in >= 2;
}

/* Append a gate. All wire arguments are already indices. */
static int add_gate(GateType t, int out, const int *in, int n_in, int line)
{
    if (n_gates >= MAX_GATES)
    {
        fprintf(stderr, "line %d: too many gates (max %d)\n", line, MAX_GATES);
        return 0;
    }
    if (!arity_ok(t, n_in))
    {
        fprintf(stderr, "line %d: wrong number of inputs for %s (got %d)\n",
                line, GATE_NAMES[t], n_in);
        return 0;
    }
    Gate *g = &gates[n_gates++];
    g->type = t;
    g->out = out;
    g->n_in = n_in;
    g->line = line;
    for (int i = 0; i < n_in; i++)
    {
        g->in[i] = in[i];
    }
    return 1;
}

/* ---------- modules ---------- */

static Module *find_module(const char *name)
{
    for (int i = 0; i < n_modules; i++)
    {
        if (strcmp(modules[i].name, name) == 0)
        {
            return &modules[i];
        }
    }
    return NULL;
}

/* Parse "MODULE name in1 in2 -> out1 out2". strtok is already past
 * the MODULE keyword. Returns the new module, or NULL on error. */
static Module *start_module(int line)
{
    char *name = strtok(NULL, DELIMS);
    GateType dummy;
    if (name == NULL || !valid_name(name) || parse_gate_type(name, &dummy))
    {
        fprintf(stderr, "line %d: MODULE needs a valid name\n", line);
        return NULL;
    }
    if (find_module(name))
    {
        fprintf(stderr, "line %d: module '%s' defined twice\n", line, name);
        return NULL;
    }
    if (n_modules >= MAX_MODULES)
    {
        fprintf(stderr, "line %d: too many modules (max %d)\n", line, MAX_MODULES);
        return NULL;
    }

    Module *m = &modules[n_modules];
    strcpy(m->name, name);
    m->n_in = m->n_out = m->n_body = 0;

    int arrow = 0;
    char *tok;
    while ((tok = strtok(NULL, DELIMS)))
    {
        if (strcmp(tok, "->") == 0)
        {
            if (arrow)
            {
                fprintf(stderr, "line %d: more than one '->'\n", line);
                return NULL;
            }
            arrow = 1;
            continue;
        }
        if (!valid_name(tok))
        {
            fprintf(stderr, "line %d: invalid port name '%s'\n", line, tok);
            return NULL;
        }
        if ((arrow ? m->n_out : m->n_in) >= MAX_PORTS)
        {
            fprintf(stderr, "line %d: too many ports (max %d)\n", line, MAX_PORTS);
            return NULL;
        }
        if (arrow)
        {
            strcpy(m->out[m->n_out++], tok);
        }
        else
        {
            strcpy(m->in[m->n_in++], tok);
        }
    }
    if (!arrow || m->n_in == 0 || m->n_out == 0)
    {
        fprintf(stderr, "line %d: MODULE format is 'MODULE name ins -> outs'\n", line);
        return NULL;
    }
    n_modules++;
    return m;
}

/* Check one line of a module body when it is defined, so mistakes are
 * reported at the module, not at every place it is used. */
static int check_body_line(const char *raw, int line)
{
    char buf[MAX_LINE];
    strcpy(buf, raw);

    char *tok = strtok(buf, DELIMS);
    GateType t;
    if (!parse_gate_type(tok, &t))
    {
        fprintf(stderr, "line %d: only gates are allowed inside a MODULE (got '%s')\n",
                line, tok);
        return 0;
    }
    int n = 0;
    while ((tok = strtok(NULL, DELIMS)))
    {
        if (!valid_name(tok))
        {
            fprintf(stderr, "line %d: invalid wire name '%s'\n", line, tok);
            return 0;
        }
        n++;
    }
    if (n == 0 || n - 1 > MAX_GATE_INPUTS || !arity_ok(t, n - 1))
    {
        fprintf(stderr, "line %d: wrong number of wires for %s\n", line, GATE_NAMES[t]);
        return 0;
    }
    return 1;
}

/* Map a wire name used inside a module body to a real wire:
 * ports become the wires passed to INSTANCE, everything else becomes
 * a private wire called "<instance>.<name>". */
static int map_wire(const Module *m, const char *local, const int *act_in,
                    const int *act_out, const char *inst, int line)
{
    for (int i = 0; i < m->n_in; i++)
    {
        if (strcmp(m->in[i], local) == 0) return act_in[i];
    }
    for (int i = 0; i < m->n_out; i++)
    {
        if (strcmp(m->out[i], local) == 0) return act_out[i];
    }
    char full[MAX_NAME * 2 + 2];
    snprintf(full, sizeof full, "%s.%s", inst, local);
    return find_or_add_wire(full, line, 0);
}

/* Parse "INSTANCE module name ins -> outs" and copy the module's gates
 * into the circuit. strtok is already past the INSTANCE keyword. */
static int expand_instance(int line)
{
    char *mname = strtok(NULL, DELIMS);
    char *iname = strtok(NULL, DELIMS);
    if (mname == NULL || iname == NULL)
    {
        fprintf(stderr, "line %d: INSTANCE format is 'INSTANCE module name ins -> outs'\n", line);
        return 0;
    }
    Module *m = find_module(mname);
    if (m == NULL)
    {
        fprintf(stderr, "line %d: unknown module '%s' (define it before use)\n", line, mname);
        return 0;
    }
    if (!valid_name(iname))
    {
        fprintf(stderr, "line %d: invalid instance name '%s'\n", line, iname);
        return 0;
    }

    int act_in[MAX_PORTS], act_out[MAX_PORTS];
    int ni = 0, no = 0, arrow = 0;
    char *tok;
    while ((tok = strtok(NULL, DELIMS)))
    {
        if (strcmp(tok, "->") == 0)
        {
            arrow++;
            continue;
        }
        if ((arrow ? no : ni) >= MAX_PORTS)
        {
            fprintf(stderr, "line %d: too many connections\n", line);
            return 0;
        }
        int w = find_or_add_wire(tok, line, 1);
        if (w < 0) return 0;
        if (arrow) act_out[no++] = w;
        else       act_in[ni++] = w;
    }
    if (arrow != 1 || ni != m->n_in || no != m->n_out)
    {
        fprintf(stderr, "line %d: module '%s' needs %d inputs -> %d outputs (got %d -> %d)\n",
                line, m->name, m->n_in, m->n_out, ni, no);
        return 0;
    }

    /* All of this line's tokens are used up, so it is safe to start
     * a fresh strtok on each stored body line. */
    for (int b = 0; b < m->n_body; b++)
    {
        char buf[MAX_LINE];
        strcpy(buf, m->body[b]);

        GateType t;
        parse_gate_type(strtok(buf, DELIMS), &t);   /* checked at definition */

        int idx[MAX_GATE_INPUTS + 1];
        int n = 0;
        while ((tok = strtok(NULL, DELIMS)))
        {
            idx[n] = map_wire(m, tok, act_in, act_out, iname, line);
            if (idx[n] < 0) return 0;
            n++;
        }
        if (!add_gate(t, idx[0], idx + 1, n - 1, line)) return 0;
    }
    return 1;
}

/* ---------- step 1: parsing ---------- */

static int parse_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        perror(path);
        return 0;
    }

    char buf[MAX_LINE], raw[MAX_LINE];
    int line = 0, ok = 1;
    Module *cur = NULL;     /* module currently being defined, if any */
    int mod_line = 0;

    while (fgets(buf, sizeof buf, f))
    {
        line++;

        char *hash = strchr(buf, '#');      /* strip comments */
        if (hash) *hash = '\0';
        strcpy(raw, buf);                   /* untouched copy for modules */

        char *tok = strtok(buf, DELIMS);
        if (tok == NULL) continue;          /* blank line */

        /* Inside MODULE ... END we just collect lines. */
        if (cur != NULL)
        {
            if (same_word(tok, "END"))
            {
                if (cur->n_body == 0)
                {
                    fprintf(stderr, "line %d: module '%s' has no gates\n", line, cur->name);
                    ok = 0;
                }
                cur = NULL;
            }
            else if (cur->n_body >= MAX_MOD_LINES)
            {
                fprintf(stderr, "line %d: module too long (max %d gates)\n", line, MAX_MOD_LINES);
                ok = 0;
            }
            else if (check_body_line(raw, line))
            {
                strcpy(cur->body[cur->n_body++], raw);
            }
            else
            {
                ok = 0;
            }
            continue;
        }

        if (same_word(tok, "MODULE"))
        {
            cur = start_module(line);
            mod_line = line;
            if (cur == NULL)
            {
                ok = 0;
                break;      /* can't tell where the bad module ends */
            }
        }
        else if (same_word(tok, "END"))
        {
            fprintf(stderr, "line %d: END without MODULE\n", line);
            ok = 0;
        }
        else if (same_word(tok, "INSTANCE"))
        {
            if (!expand_instance(line)) ok = 0;
        }
        else if (same_word(tok, "INPUT"))
        {
            while ((tok = strtok(NULL, DELIMS)))
            {
                int w = find_or_add_wire(tok, line, 1);
                if (w < 0) { ok = 0; break; }
                if (wires[w].is_input)
                {
                    fprintf(stderr, "line %d: input '%s' declared twice\n", line, tok);
                    ok = 0;
                    continue;
                }
                if (n_inputs >= MAX_INPUTS)
                {
                    fprintf(stderr, "line %d: too many inputs (max %d)\n", line, MAX_INPUTS);
                    ok = 0;
                    break;
                }
                wires[w].is_input = 1;
                inputs[n_inputs++] = w;
            }
        }
        else if (same_word(tok, "OUTPUT"))
        {
            while ((tok = strtok(NULL, DELIMS)))
            {
                int w = find_or_add_wire(tok, line, 1);
                if (w < 0) { ok = 0; break; }
                if (wires[w].is_output)
                {
                    fprintf(stderr, "line %d: output '%s' declared twice\n", line, tok);
                    ok = 0;
                    continue;
                }
                wires[w].is_output = 1;
                outputs[n_outputs++] = w;
            }
        }
        else
        {
            GateType t;
            if (!parse_gate_type(tok, &t))
            {
                fprintf(stderr, "line %d: unknown gate or keyword '%s'\n", line, tok);
                ok = 0;
                continue;
            }
            int idx[MAX_GATE_INPUTS + 1];
            int n = 0, bad = 0;
            while ((tok = strtok(NULL, DELIMS)))
            {
                if (n > MAX_GATE_INPUTS)
                {
                    fprintf(stderr, "line %d: too many gate inputs (max %d)\n",
                            line, MAX_GATE_INPUTS);
                    bad = 1;
                    break;
                }
                idx[n] = find_or_add_wire(tok, line, 1);
                if (idx[n] < 0) { bad = 1; break; }
                n++;
            }
            if (bad) { ok = 0; continue; }
            if (n == 0)
            {
                fprintf(stderr, "line %d: gate has no output wire\n", line);
                ok = 0;
                continue;
            }
            if (!add_gate(t, idx[0], idx + 1, n - 1, line)) ok = 0;
        }
    }

    if (cur != NULL)
    {
        fprintf(stderr, "line %d: MODULE '%s' is missing END\n", mod_line, cur->name);
        ok = 0;
    }
    fclose(f);
    return ok;
}

/* ---------- step 2: validation ---------- */

static int validate(void)
{
    int ok = 1;

    if (n_inputs == 0)  { fprintf(stderr, "error: no INPUT declared\n");  ok = 0; }
    if (n_outputs == 0) { fprintf(stderr, "error: no OUTPUT declared\n"); ok = 0; }

    /* Every wire may have at most one driver, and inputs have none. */
    for (int g = 0; g < n_gates; g++)
    {
        Wire *w = &wires[gates[g].out];
        if (w->is_input)
        {
            fprintf(stderr, "line %d: gate drives input '%s'\n", gates[g].line, w->name);
            ok = 0;
        }
        else if (w->driver != -1)
        {
            fprintf(stderr, "line %d: wire '%s' already driven by gate on line %d\n",
                    gates[g].line, w->name, gates[w->driver].line);
            ok = 0;
        }
        else
        {
            w->driver = g;
        }
    }

    /* Every non-input wire must be driven by something. */
    for (int i = 0; i < n_wires; i++)
    {
        if (!wires[i].is_input && wires[i].driver == -1)
        {
            fprintf(stderr, "error: wire '%s' is never driven\n", wires[i].name);
            ok = 0;
        }
    }
    return ok;
}

/* ---------- step 3: evaluation order (topological sort) ----------
 *
 * A gate can only be evaluated once every gate feeding it has been
 * evaluated. Kahn's algorithm: start with gates whose inputs are all
 * circuit inputs, then repeatedly "release" gates whose dependencies
 * are done. If some gates are never released, there is a loop
 * (combinational feedback), which this simulator does not support. */

static int topo_sort(void)
{
    int pending[MAX_GATES];     /* unevaluated gate inputs of gate g */
    int queue[MAX_GATES];
    int head = 0, tail = 0, count = 0;

    for (int g = 0; g < n_gates; g++)
    {
        pending[g] = 0;
        for (int i = 0; i < gates[g].n_in; i++)
        {
            if (wires[gates[g].in[i]].driver != -1)
            {
                pending[g]++;
            }
        }
        if (pending[g] == 0)
        {
            queue[tail++] = g;
        }
    }

    while (head < tail)
    {
        int g = queue[head++];
        order[count++] = g;
        for (int h = 0; h < n_gates; h++)
        {
            for (int i = 0; i < gates[h].n_in; i++)
            {
                if (gates[h].in[i] == gates[g].out && --pending[h] == 0)
                {
                    queue[tail++] = h;
                }
            }
        }
    }

    if (count < n_gates)
    {
        fprintf(stderr, "error: combinational loop detected involving:\n");
        for (int g = 0; g < n_gates; g++)
        {
            if (pending[g] > 0)
            {
                fprintf(stderr, "  gate on line %d (output '%s')\n",
                        gates[g].line, wires[gates[g].out].name);
            }
        }
        return 0;
    }
    return 1;
}

/* ---------- step 4: simulation ---------- */

static int eval_gate(const Gate *g)
{
    int v = wires[g->in[0]].value;

    if (g->type == G_BUF) return v;
    if (g->type == G_NOT) return !v;

    for (int i = 1; i < g->n_in; i++)
    {
        int x = wires[g->in[i]].value;
        switch (g->type)
        {
            case G_AND: case G_NAND: v &= x; break;
            case G_OR:  case G_NOR:  v |= x; break;
            case G_XOR: case G_XNOR: v ^= x; break;
            default: break;
        }
    }
    if (g->type == G_NAND || g->type == G_NOR || g->type == G_XNOR)
    {
        v = !v;
    }
    return v;
}

/* Input values must already be set. One pass in topological order
 * computes every wire, because each gate's inputs are ready first. */
static void simulate(void)
{
    for (int k = 0; k < n_gates; k++)
    {
        Gate *g = &gates[order[k]];
        wires[g->out].value = eval_gate(g);
    }
}

static void print_header(void)
{
    for (int i = 0; i < n_inputs; i++)  printf("%s ", wires[inputs[i]].name);
    printf("| ");
    for (int i = 0; i < n_outputs; i++) printf("%s ", wires[outputs[i]].name);
    printf("\n");
}

static void print_row(void)
{
    /* Right-align each value under its column name. */
    for (int i = 0; i < n_inputs; i++)
    {
        printf("%*d ", (int) strlen(wires[inputs[i]].name), wires[inputs[i]].value);
    }
    printf("| ");
    for (int i = 0; i < n_outputs; i++)
    {
        printf("%*d ", (int) strlen(wires[outputs[i]].name), wires[outputs[i]].value);
    }
    printf("\n");
}

static void print_truth_table(void)
{
    print_header();
    long rows = 1L << n_inputs;
    for (long combo = 0; combo < rows; combo++)
    {
        /* First declared input is the most significant bit. */
        for (int i = 0; i < n_inputs; i++)
        {
            wires[inputs[i]].value = (combo >> (n_inputs - 1 - i)) & 1;
        }
        simulate();
        print_row();
    }
}

/* ---------- extras: test vectors and stats ---------- */

/* Apply "NAME=0/1" arguments. Every input must be given exactly once. */
static int apply_vector(char **args, int n)
{
    int given[MAX_WIRES] = {0};
    int ok = 1;

    for (int a = 0; a < n; a++)
    {
        char name[MAX_NAME];
        char *eq = strchr(args[a], '=');
        size_t len = eq - args[a];
        if (len == 0 || len >= MAX_NAME || strlen(eq + 1) != 1 ||
            (eq[1] != '0' && eq[1] != '1'))
        {
            fprintf(stderr, "error: bad assignment '%s' (use NAME=0 or NAME=1)\n", args[a]);
            ok = 0;
            continue;
        }
        memcpy(name, args[a], len);
        name[len] = '\0';

        int w = find_wire(name);
        if (w < 0 || !wires[w].is_input)
        {
            fprintf(stderr, "error: '%s' is not an input of this circuit\n", name);
            ok = 0;
        }
        else if (given[w])
        {
            fprintf(stderr, "error: '%s' given twice\n", name);
            ok = 0;
        }
        else
        {
            given[w] = 1;
            wires[w].value = eq[1] - '0';
        }
    }

    for (int i = 0; i < n_inputs; i++)
    {
        if (!given[inputs[i]])
        {
            fprintf(stderr, "error: no value given for input '%s'\n", wires[inputs[i]].name);
            ok = 0;
        }
    }
    return ok;
}

/* Depth = number of gates on the longest path from any input to any
 * output: a rough stand-in for how slow the circuit would be. */
static void print_stats(void)
{
    int level[MAX_GATES];
    int count[G_COUNT] = {0};
    int depth = 0;

    for (int k = 0; k < n_gates; k++)
    {
        int g = order[k];
        int lv = 0;
        for (int i = 0; i < gates[g].n_in; i++)
        {
            int d = wires[gates[g].in[i]].driver;
            if (d >= 0 && level[d] > lv) lv = level[d];
        }
        level[g] = lv + 1;
        count[gates[g].type]++;
    }
    for (int i = 0; i < n_outputs; i++)
    {
        int d = wires[outputs[i]].driver;
        if (d >= 0 && level[d] > depth) depth = level[d];
    }

    printf("\n-- stats --\n");
    printf("gates:   %d (", n_gates);
    int first = 1;
    for (int t = 0; t < G_COUNT; t++)
    {
        if (count[t])
        {
            printf("%s%s %d", first ? "" : ", ", GATE_NAMES[t], count[t]);
            first = 0;
        }
    }
    printf(")\n");
    printf("wires:   %d (%d inputs, %d outputs)\n", n_wires, n_inputs, n_outputs);
    printf("modules: %d defined\n", n_modules);
    printf("depth:   %d gate levels on the longest input to output path\n", depth);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s circuit.circ [NAME=0|1 ...] [--stats]\n", argv[0]);
        return 1;
    }

    /* Sort the extra arguments into flags and assignments. */
    int stats = 0, n_vec = 0;
    char *vec[MAX_INPUTS * 2];
    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--stats") == 0)
        {
            stats = 1;
        }
        else if (strchr(argv[i], '=') && n_vec < MAX_INPUTS * 2)
        {
            vec[n_vec++] = argv[i];
        }
        else
        {
            fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!parse_file(argv[1])) return 1;
    if (!validate())          return 1;
    if (!topo_sort())         return 1;

    if (n_vec > 0)
    {
        if (!apply_vector(vec, n_vec)) return 1;
        simulate();
        print_header();
        print_row();
    }
    else
    {
        print_truth_table();
    }

    if (stats) print_stats();
    return 0;
}
