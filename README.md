# cy370dump

cy370 decoder tool by Darksoft v1.00. Use at your own risk.

`cy370dump` reads the JEDEC fuse file (`.jed`) of a Cypress FLASH370 / FLASH370i CPLD and writes a VHDL design, with
every signal fixed to its package pin, that can be compiled again with Cypress Warp for the same part. Built for arcade
PCB preservation: read the device, decode the fuse file, keep the logic as source.

The program is a single executable. It needs no vendor software and no data files to decode.

## Supported parts and packages

| part | packages (code: order codes) |
|---|---|
| CY7C371, CY7C371i | A: -143AC -110AC -83AC -83AI -66AI -66AC; J: -143JC -110JC -83JC -83JI -66JI -66JC; Y: -83YMB -66YMB |
| CY7C372, CY7C372i | J: -125JC -100JC -83JC -83JI -66JC -66JI; Y: -83YMB -66YMB |
| CY7C373, CY7C373i | A: -125AC -100AC -83AC -83AI -66AC -66AI; J: -125JC -100JC -83JC -83JI -66JC -66JI |
| CY7C374 | A: -100AC -83AC -83AI -66AC -66AI; G: -100GC -83GC -83GMB -66GC -66GMB; J: -100JC -83JC -83JI -66JC -66JI; Y: -83YMB -66YMB |
| CY7C374i | A, J, Y as CY7C374 |
| CY7C375, CY7C375i | A: -100AC -83AC -83AI -66AC -66AI; G: -83GMB -66GMB; U: -83UMB -66UMB |

`cy370dump devices` prints the same list from the program's own tables.

A fuse file does not always say which package it was made for. Files written by Warp carry a line
`NOTE PACKAGE <order code>`; the program then uses that package. For a file without the note (for example one read
back from a device by a programmer) give the package with `--package`.

## Commands

    cy370dump
        prints the command list

    cy370dump devices
        lists every supported part and package with its order codes

    cy370dump decode <file.jed> --viewlist
        lists the packages the part named in the fuse file can be decoded for

    cy370dump decode <file.jed> <out.vhd> [--top NAME] [--package P] [--pinmap FILE]
        writes the recovered design
        --top NAME      entity name (default: design)
        --package P     package code: A, G, J, U or Y
        --pinmap FILE   also writes one line "<package pin> <I/O-cell label>" per pin of the design

Examples:

    cy370dump decode board_u12.jed u12.vhd --top u12
    cy370dump decode readback.jed u7.vhd --top u7 --package J --pinmap u7_pins.txt

Exit code 0: the design was written. Exit code 1: the file was refused; the first line printed is
`REFUSED: <NAME> <detail>` and nothing is written. Exit code 2: wrong command line.

## What is printed

    decoded board_u12.jed: 28 macrocells, 37 ports
    PACKAGE J source note
    USED <field> <value>            every configuration value found in the file
    RACING q<n> clock p<k>          a register whose clock pin is also an input of its own logic
    KEEP_CONSTANT ...               a register or latch written through a keep form (see below)
    INPUT_CELL ...                  an input register or latch on a dedicated input
    WARNING ...                     the design was written, but Warp may refuse to fit it
    INVERTED_REGISTER_PINS <pins>   registered outputs whose pin shows the complement of the stored bit

## The recovered design

Signals are named after the package pins: `p<n>` is pin n, `q<n>` the macrocell behind pin n (or a buried macrocell,
which keeps its node number), `i<n>` an input register or latch on dedicated input n. Each combinational macrocell is
one assignment, each register or latch one process (asynchronous reset first, then preset), each driven pin one
assignment with its output-enable term. The `pin_numbers` attribute fixes every signal to its pin, and the `polarity`
attribute (`pl_keep`) tells Warp to keep the stored polarity of every register and latch, so that the power-up state of
the recompiled device is the one of the original.

A register or latch whose next state is a constant, or its own output, would be optimised away by Warp together with
its power-up value. Such a cell is written through another signal of the design so that Warp keeps it (the lines
`KEEP_CONSTANT` name each one); if no pin signal can carry it, a small internal signal `kc` is added in a free buried
macrocell. This changes no pin of the design.

## Recompiling with Warp

The output is VHDL for Cypress Warp (release 4). Compile it for the same part and package:

    warp -d c371 -p CY7C371-143JC u12.vhd

Use the device name (`c371` .. `c375`, `c371i` .. `c375i`) and one order code of the package the design was decoded
for; both are written in the first lines of the `.vhd` file. Warp must place every signal on the pin the file asks
for; if its report shows a signal moved or a fit error, the result is not a copy of the original.

After fitting, decode the new fuse file with `cy370dump` too and compare the two `.vhd` files. A good copy shows the
same equations on the same pins.

## Known limits: files the program refuses

The program writes a design only when every field and every construct in the fuse file is one it handles in a way that
has been checked against the vendor tools. Anything else is refused by name and nothing is written.

Fuse file defects: the file is damaged, incomplete or not a FLASH370 fuse file. Read the device again.
`NO_STX`, `MULTIPLE_STX`, `NO_ETX`, `MULTIPLE_ETX` (start / end marks), `MISSING_TRANSMISSION_CHECKSUM`,
`BAD_TRANSMISSION_CHECKSUM`, `FUSE_CHECKSUM_MISSING`, `BAD_FUSE_CHECKSUM` (the two checksums), `BAD_LINE_END`,
`BAD_FIELD`, `UNKNOWN_FIELD`, `MIXED_FORM` (binary and hex fuse records in one file), `QF_MISSING`, `QF_MISMATCH`,
`FUSE_COUNT` (the fuse count is absent or not the part's), `BAD_FUSE_DIGIT`, `HEX_RECORD_LENGTH`, `HEX_PADDING`,
`FUSE_INDEX_OUT_OF_RANGE`, `FUSE_DEFINED_TWICE`. `CANNOT_READ` / `CANNOT_WRITE` name a file that could not be opened.

Package: `PACKAGE_NOT_GIVEN` (give `--package`), `PACKAGE_MISMATCH` (`--package` disagrees with the file's note),
`PACKAGE_NOT_OF_DEVICE`, `PACKAGE_NOTE_NOT_OF_DEVICE`, `DEVICE_NOT_SUPPORTED`.

Logic the program does not recover:

| name | what the device contains | why it is refused |
|---|---|---|
| `CLOCK_PIN_HOLD_LOOP_INVERTED` | a combinational macrocell that holds its own value on a clock-pin literal (a transparent latch built as a loop) and drives its pin inverted | Warp may store the cell complemented when it fits the recovered design, with no warning |
| `LATCH_SELF_FEEDBACK` | a latch whose input reads the latch itself | Warp may rebuild the cell as a loop stored in the other polarity |
| `CYCLE_HOLD_PAD_OE` | a loop of two or more combinational macrocells with a hold-only member, a pin read inside the loop, and an output enable that depends on the loop | Warp may store the hold node complemented |
| `STRUCTURE_NOT_SUPPORTED OE_PAD_LOOP` | an output enable that depends, with no register in between, on the pin it enables | the pin has no defined steady value; the vendor simulation oscillates |
| `STRUCTURE_NOT_SUPPORTED PAD_SELF_LOOP_NOT_SETTLED`, `COMB_SELF_LOOP_NOT_SETTLED` | a combinational macrocell that reads its own pin (or its own output) through an odd number of inversions | for some input values the cell has no steady value (a ring oscillator) |
| `STRUCTURE_NOT_SUPPORTED COMB_HOLD_SELF_ONLY` | a combinational macrocell whose only input is its own output (it holds whatever value it has; no input can change it) | Warp removes such a cell when it fits the recovered design and leaves its pin undriven, with no message |
| `STRUCTURE_NOT_SUPPORTED LATCH_SELF_NEGATION` | a transparent latch that reads its own complement | it oscillates while transparent |
| `STRUCTURE_NOT_SUPPORTED IREG_IN_ASYNC` | an input register or latch read by an asynchronous reset or preset term | Warp cannot fit this form from source |
| `CIPOL_NOT_SUPPORTED_ON_c373`, `INPUT_CELL_POLARITY_UNMODELLED` | CY7C373 / 373i: an input register clocked on the falling edge (or an input latch transparent while low) | the vendor fitter and the vendor simulation model disagree on this field for this part |
| `REFIT_INPUT_REGISTER_CONVERSION_c373` | CY7C373 / 373i: a falling-edge register fed directly by one dedicated input, in the form Warp turns into an input register | the result falls under the previous line |
| `INPUT_CELL_ON_CLOCK_PIN_NO_CI` | CY7C371 / 372: an input register on a clock pin | the part has no field that says which clock drives it |
| `IP_PIN_INPUT_NOT_MODELLED` | a buried macrocell that takes its input from the neighbouring pin | not handled |
| `<FIELD>_NOT_SUPPORTED`, `<FIELD>_NOT_SUPPORTED_ON_<part>` (for example `REG2_NOT_SUPPORTED REG2=T`, `FPOL_NOT_SUPPORTED`, `BURIED_NOT_SUPPORTED`, `IREG2_NOT_SUPPORTED_ON_c374`) | a configuration value the program does not handle, in general or on that part, such as a T flip-flop | not handled |
| `VPP_PIN_READ`, `VPP_PIN_INPUT_CELL` | an "i" part whose logic uses the VPP pin | that pin is not a user pin on the "i" parts |
| `KEEP_...` (`KEEP_CONSTANT_NO_FREE_NODE`, `KEEP_CONSTANT_LATCH_SHARED`, `KEEP_HOLD_NO_NODE`, ...), `CONSTANT_TRUE_RESET`, `CONSTANT_ENABLE_BANK`, `LITERAL_ON_UNUSED_PIM_INPUT`, `PIM_CODE_NOT_MODELLED`, `PIM_SLOT_NC`, `FEEDBACK_OF_UNUSED_MACROCELL`, `EMPTY_DESIGN` | fuse patterns Warp itself does not write, or a constant register that no signal of the design can carry | usually a damaged or foreign file |

Two warnings do not stop the program: `WARNING POLARITY_COPY_CONFLICT` and `WARNING CLOCK_PIN_HOLD_LOOP`. The design
is written and is a correct description of the device, but Warp may answer "Preplaced signals use conflicting clock
polarity" and refuse to fit it.

Designs with `RACING` lines contain a register whose clock is also one of its data inputs. The recovered equations are
those of the device, but such a circuit depends on internal delays, and a recompiled part may behave differently at
that register.

## Building from source

    make                 Linux:   gcc -std=c99 -Wall -Wextra -Werror -O2
    make cy370dump.exe   Windows: x86_64-w64-mingw32-gcc, static, 32 MB stack

The source is plain C99 with no dependencies beyond the C library. `src/tables.c` holds the device tables.

## License
Copyright (C) DARKSOFT

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See the LICENSE file.
