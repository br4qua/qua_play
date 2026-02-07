# CLAUDE.md - qua-bare-launcher

## Role

Thin process wrapper. Sets up execution environment before exec'ing the
actual player binary.

## Status

Stable. Rarely changes.

## What It Does

- Sets nice level / scheduling priority
- Redirects stdout
- Execs the target player binary with the given arguments

## Key Files

- `qua-bare-launcher.c` - Single source file, entire component
