/* The Fujitzee server's binary protocol, for the Palm client.
 *
 * The server (https://fujitzee.carr-designs.com/ by default) is plain
 * HTTP(S) and runs every rule; a client polls the table and sends moves:
 *
 *   GET <server>tables?bin=1&be=1                       the public tables
 *   GET <server>state?table=T&player=P&bin=1&be=1       join (first call) and poll
 *   GET <server>ready?...                               toggle ready while waiting
 *   GET <server>roll/MMMMM?...                          re-roll: '1' rolls that die, '0' holds it
 *   GET <server>score/N?...                             take scorecard row N (0-14)
 *   GET <server>leave?...                               give up the seat
 *
 * Every call replies with the state. bin=1 asks for the packed structs of
 * src/misc.h; be=1 makes the int16 scores big-endian. Strings are fixed-size,
 * NUL-padded and lowercased. The reply is read field by field, since a 68000
 * cannot read an int16 at the odd offsets the packed layout puts them at.
 *
 * Portable C89, with no libc, for the Palm and the desktop test.
 */
#ifndef FZWIRE_H
#define FZWIRE_H

#include "fn_types.h"

typedef short fz_s16;

#define FZ_NAME_LEN        9
#define FZ_SERVER_NAME_LEN 21
#define FZ_PROMPT_LEN      41
#define FZ_DICE_LEN        6
#define FZ_PLAYER_MAX      12
#define FZ_SLOTS           16   /* scorecard slots in a player record */
#define FZ_CHOICES         15   /* validScores: slots 0-14 */
#define FZ_TABLE_ID_LEN    9
#define FZ_TABLE_NAME_LEN  21
#define FZ_TABLE_SEATS_LEN 6
#define FZ_MAX_TABLES      10

/* Wire sizes: a state is the header and playerCount players. */
#define FZ_STATE_HEADER 95
#define FZ_PLAYER_SIZE  42
#define FZ_STATE_MAX (FZ_STATE_HEADER + FZ_PLAYER_MAX * FZ_PLAYER_SIZE) /* 599 */
#define FZ_TABLE_SIZE   36
#define FZ_TABLES_MAX (1 + FZ_MAX_TABLES * FZ_TABLE_SIZE)               /* 361 */

/* round: 0 is the ready-up room, 1-13 play, FZ_ROUND_OVER the final card. */
#define FZ_ROUND_OVER 99
#define FZ_ROUNDS     13

/* Scorecard slots, in the wire's order. */
#define FZ_UPPER_TOTAL 6
#define FZ_BONUS       7
#define FZ_TOTAL       15

/* scores[] values: an empty row, and in round 0 a spectator's slot 0. */
#define FZ_EMPTY     (-1)
#define FZ_SPECTATOR (-2)

typedef struct {
    char name[FZ_NAME_LEN];
    fn_u8 alias;
    fz_s16 scores[FZ_SLOTS];
} FzPlayer;

typedef struct {
    fn_u8 playerCount;
    char serverName[FZ_SERVER_NAME_LEN];
    char prompt[FZ_PROMPT_LEN];
    fn_u8 round;
    fn_u8 rollsLeft;
    signed char activePlayer;     /* -1 for none */
    fn_u8 moveTime;               /* seconds left on the move clock */
    fn_u8 viewing;                /* this client is a spectator */
    char dice[FZ_DICE_LEN];       /* "21513", or "" before the first roll */
    char keepRoll[FZ_DICE_LEN];   /* the mask of the last roll sent */
    signed char validScores[FZ_CHOICES]; /* points a row would earn; -1 closed */
    FzPlayer players[FZ_PLAYER_MAX];
} FzGame;

typedef struct {
    char id[FZ_TABLE_ID_LEN];
    char name[FZ_TABLE_NAME_LEN];
    char seats[FZ_TABLE_SEATS_LEN];   /* "1 / 6" */
} FzTable;

typedef struct {
    fn_u8 count;
    FzTable tables[FZ_MAX_TABLES];
} FzTables;

/* Both return 0 unless len is exactly the size the counts in buf imply. */
int fz_parse_game(const fn_u8 *buf, fn_u16 len, FzGame *game);
int fz_parse_tables(const fn_u8 *buf, fn_u16 len, FzTables *tables);

/* "N1:<server><path>?table=T&player=P&bin=1&be=1"; https:// is added to a
 * server given without a scheme. 0 if it did not fit in size. */
int fz_build_url(char *out, fn_u16 size, const char *server, const char *path,
                 const char *table, const char *player);
/* A lobby address "https://host/?table=T" is cut at '?' and T copied out;
 * 0 if server held no table. */
int fz_split_server(char *server, char *table, fn_u16 tableSize);
/* Keeps letters, digits and single inner spaces, up to 8; returns the length. */
fn_u16 fz_clean_name(char *name);
/* The name to show: a bot's leading digit (its initial on the 8-bit
 * clients) is dropped, so "1ai clyd" reads "ai clyd". */
const char *fz_display_name(const FzPlayer *player);

/* The running total: upper total, bonus and every lower row taken so far.
 * The server fills slot 15 only at game over. */
fz_s16 fz_total(const FzPlayer *player);
/* The open row (0-14) worth the most, or -1 when there is none. */
int fz_best_row(const FzGame *game);
/* True for a row a player may take: not the upper total or the bonus. */
int fz_selectable(int row);
/* Two-letter and long labels for slot 0-15. */
const char *fz_short_label(int slot);
const char *fz_label(int slot);

#endif /* FZWIRE_H */
