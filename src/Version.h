#pragma once
// Single source of truth for the shipped version. Bump alongside the
// v-prefixed commit messages; the .rc bakes these into the exe's
// VERSIONINFO resource (anonymous binaries score worse with every AV
// heuristic on earth).
#define TANKAQ_VER_MAJOR 0
#define TANKAQ_VER_MINOR 41
#define TANKAQ_VER_PATCH 5
#define TANKAQ_VER_STRING "0.41.5"
