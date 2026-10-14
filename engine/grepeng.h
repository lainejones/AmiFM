#ifndef GREPENG_H
#define GREPENG_H

/*
 * AmiGrep's engine (grep.h) under names of its own. finder.h and grep.h both
 * declare struct Matcher, matcherInit and PollFunc; AmiFM uses both engines,
 * so the grep side is renamed here (and in grepeng.c) while grep.c and grep.h
 * stay byte-identical copies of AmiGrep's files.
 */

#define Matcher     GrepMatcher
#define matcherInit grepMatcherInit
#define PollFunc    GrepPollFunc
#include "grep.h"
#undef Matcher
#undef matcherInit
#undef PollFunc

#endif /* GREPENG_H */
