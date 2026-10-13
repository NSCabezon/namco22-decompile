/* tc_game_core.c -- src/tc_game.c for the visionOS core library (CMake target tc_core): the same file, its main() named tc_main
 * (vision/tc_core.m's game thread calls it). A wrapper rather than a per-source definition, so the tc / tc_metal executables built
 * from the same directory keep their main. */
#define main tc_main
#include "../src/tc_game.c"
