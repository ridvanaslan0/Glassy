# Contributing to Glassy

Thanks for your interest in improving Glassy!

## Reporting bugs

Please open an issue using the **Bug report** template and include:

- Your Windows version (for example Windows 11 23H2) and display setup (resolution, scaling, number of monitors)
- What you expected and what happened instead
- Steps to reproduce, and a screenshot if it is a visual problem

## Suggesting features

Open an issue with the **Feature request** template. Small, focused ideas that keep Glassy fast and dependency-free are the easiest to accept.

## Building

See the *Build from source* section of the [README](README.md). You need MinGW-w64 and nothing else.

## Pull requests

- Keep the project a single dependency-free C file unless there is a strong reason to split it.
- Keep the code and all comments in English.
- The build must succeed with no warnings (`-Wall`).
- Never block the hook thread: the mouse hook callback must stay tiny and must not draw.
- Describe what you changed and how you tested it. Add an entry to `CHANGELOG.md` under *Unreleased*.

## Code style

- C99-compatible, 4-space indentation, no tabs.
- Keep comments short and explain the *why*, not the *what*.
