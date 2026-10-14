# engine/

The search engines AmiFM's Find and Grep use, copied unchanged from their own projects:

| File | From |
|---|---|
| `finder.c`, `finder.h` | [AmiFind](https://github.com/lainejones/AmiFind) `src/` |
| `grep.c`, `grep.h` | [AmiGrep](https://github.com/lainejones/AmiGrep) `src/` |

Both declare `struct Matcher`, `matcherInit` and `PollFunc`, so the grep engine is compiled
through `grepeng.c` / `grepeng.h`, which rename those three to `GrepMatcher`,
`grepMatcherInit` and `GrepPollFunc`. Keep the four copied files identical to their
originals: a fix belongs in AmiFind or AmiGrep first, then copy the file here again.
