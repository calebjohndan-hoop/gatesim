# GateSim: a gate-level digital logic simulator in C
#### Description:

GateSim is a command line program written in C that simulates digital logic circuits at the level of individual gates. You describe a circuit in a small plain text file, listing its inputs, its outputs and the gates that connect them, and GateSim checks the circuit for mistakes, works out the correct order to evaluate the gates in, and prints the complete truth table. It can also simulate a single set of input values, report statistics about the circuit, and let you define a sub-circuit once as a module and reuse it many times.

I am a first-year Electronics and Communication Engineering student and I want to work in VLSI. Professional chip design tools like ModelSim or Vivado are huge and hide what a simulator actually does. I wanted to understand that from the inside, so I built a small one myself. GateSim is also genuinely useful for checking digital electronics homework: you can type in a circuit from class and see its truth table in seconds.

## The circuit file format

A circuit file has one statement per line, and anything after `#` is a comment. A half adder looks like this:

    INPUT  A B
    OUTPUT SUM CARRY
    XOR    SUM   A B
    AND    CARRY A B

Each gate line is the gate type, then the output wire, then the input wires. GateSim supports AND, OR, XOR, NAND, NOR and XNOR with two to eight inputs, plus NOT and BUF with exactly one. Keywords are case insensitive. Gates may be written in any order, because GateSim sorts them itself.

Modules let you build bigger circuits from smaller ones:

    MODULE FA A B CIN -> SUM COUT
        ...gates...
    END
    INSTANCE FA U0 A0 B0 CIN -> S0 C1

The file `examples/adder4.circ` builds a 4-bit ripple carry adder from four instances of one full adder module.

## How to use it

    make
    ./gatesim examples/full_adder.circ                    # full truth table
    ./gatesim examples/full_adder.circ A=1 B=1 CIN=0      # one test vector
    ./gatesim examples/adder4.circ --stats                # circuit report
    make test                                              # run the test suite

## Files

**gatesim.c** contains the whole simulator. It works in four stages. The parser (`parse_file`) reads the file line by line using `strtok`, strips comments, and turns each statement into entries in arrays of `Wire` and `Gate` structs. Module definitions are stored as text and copied into the circuit each time an `INSTANCE` line appears (`expand_instance`), with internal wires renamed to `instance.wire` so two copies never collide. The validator (`validate`) rejects broken circuits: wires driven by two gates, gates that try to drive an input, and wires that are used but never driven. The sorter (`topo_sort`) orders the gates so that every gate comes after the gates feeding it, and detects loops. Finally the simulator (`simulate`) evaluates the gates in that order for each row of the truth table. The file also contains test vector mode (`apply_vector`) and the statistics report (`print_stats`).

**Makefile** builds the program with all compiler warnings enabled and runs the tests.

**tests.sh** is an automated test suite. It checks that every valid example runs, that every broken example is rejected, that test vector mode behaves correctly, and that the 4-bit adder gives the right answer for all 512 possible inputs by comparing it with real arithmetic.

**examples/** holds working circuits: a half adder, a full adder with its gates deliberately out of order, a 2-to-1 multiplexer, and the 4-bit adder. **examples/errors/** holds circuits that are broken on purpose, one for each kind of error GateSim detects.

## Design choices

**Arrays and indices instead of pointers.** Each wire and gate refers to others by their index in a global array rather than by pointer. Indices are easy to print while debugging, can never dangle, and need no `malloc` or `free`. The cost is fixed limits (for example 512 gates), which are more than enough for circuits a person writes by hand.

**Topological sorting with Kahn's algorithm.** I considered simply evaluating every gate repeatedly until nothing changed. That works, but it is slower and it hides mistakes. Sorting the gates once means a single pass computes every output correctly, and if the sort cannot finish, the leftover gates are exactly the ones forming a loop, so GateSim can name them in the error message.

**Rejecting combinational loops.** Real latches and flip flops use feedback, but simulating them properly requires modelling time and delays. I decided to keep GateSim purely combinational and treat a loop as an error, which keeps the results always well defined.

**Circuit depth.** The `--stats` report counts the gates on the longest path from an input to an output. In real hardware this is roughly what limits clock speed. For the 4-bit ripple carry adder it is 9, and it grows with every bit added, which is exactly why faster adder designs like carry lookahead exist.

**Limit of 16 inputs.** A truth table doubles in size with every input, so 16 inputs already means 65,536 rows. Beyond that a full table is not useful, which is why test vector mode exists.

## What I learned and what is next

Building GateSim taught me how parsing, graphs and simulation fit together, and why real tools are designed the way they are. Next I would like to add flip flops with a clock, and a parser for a small subset of Verilog.
