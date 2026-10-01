#ifndef _FEATURES_H
#define _FEATURES_H
// Aegis replacement for musl's internal features.h.
#define _BSD_SOURCE 1
#define _XOPEN_SOURCE 700
#define _GNU_SOURCE 1
#define hidden __attribute__((__visibility__("hidden")))
#define weak __attribute__((__weak__))
#define weak_alias(old, new) extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
#endif
