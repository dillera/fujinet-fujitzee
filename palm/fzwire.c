#include "fzwire.h"

/* Wire offsets in a state reply (Game in src/misc.h). */
#define OFF_PLAYER_COUNT 0
#define OFF_SERVER_NAME  1
#define OFF_PROMPT       22
#define OFF_ROUND        63
#define OFF_ROLLS_LEFT   64
#define OFF_ACTIVE       65
#define OFF_MOVE_TIME    66
#define OFF_VIEWING      67
#define OFF_DICE         68
#define OFF_KEEP_ROLL    74
#define OFF_VALID        80

/* Within a player. */
#define P_NAME   0
#define P_ALIAS  9
#define P_SCORES 10

static void CopyField(char *dst, const fn_u8 *src, fn_u16 size)
{
    fn_u16 i;

    for (i = 0; i + 1 < size && src[i] != 0; i++)
        dst[i] = (char)src[i];
    dst[i] = '\0';
}

static fz_s16 Get16(const fn_u8 *p)
{
    return (fz_s16)(((fn_u16)p[0] << 8) | p[1]);
}

static fn_u16 Len(const char *s)
{
    fn_u16 n = 0;

    while (s[n])
        n++;
    return n;
}

int fz_parse_game(const fn_u8 *buf, fn_u16 len, FzGame *game)
{
    fn_u16 i, k;
    const fn_u8 *p;

    if (len < FZ_STATE_HEADER)
        return 0;
    game->playerCount = buf[OFF_PLAYER_COUNT];
    if (game->playerCount > FZ_PLAYER_MAX ||
        len != FZ_STATE_HEADER + (fn_u16)game->playerCount * FZ_PLAYER_SIZE)
        return 0;

    CopyField(game->serverName, buf + OFF_SERVER_NAME, FZ_SERVER_NAME_LEN);
    CopyField(game->prompt, buf + OFF_PROMPT, FZ_PROMPT_LEN);
    game->round = buf[OFF_ROUND];
    game->rollsLeft = buf[OFF_ROLLS_LEFT];
    game->activePlayer = (signed char)buf[OFF_ACTIVE];
    game->moveTime = buf[OFF_MOVE_TIME];
    game->viewing = buf[OFF_VIEWING];
    CopyField(game->dice, buf + OFF_DICE, FZ_DICE_LEN);
    CopyField(game->keepRoll, buf + OFF_KEEP_ROLL, FZ_DICE_LEN);
    for (i = 0; i < FZ_CHOICES; i++)
        game->validScores[i] = (signed char)buf[OFF_VALID + i];
    for (i = 0; i < game->playerCount; i++) {
        FzPlayer *pl = &game->players[i];

        p = buf + FZ_STATE_HEADER + i * FZ_PLAYER_SIZE;
        CopyField(pl->name, p + P_NAME, FZ_NAME_LEN);
        pl->alias = p[P_ALIAS];
        for (k = 0; k < FZ_SLOTS; k++)
            pl->scores[k] = Get16(p + P_SCORES + 2 * k);
    }
    if (game->activePlayer >= (signed char)game->playerCount)
        game->activePlayer = -1;
    return 1;
}

int fz_parse_tables(const fn_u8 *buf, fn_u16 len, FzTables *tables)
{
    fn_u16 i;
    const fn_u8 *p;

    if (len < 1 || buf[0] > FZ_MAX_TABLES || len != 1 + (fn_u16)buf[0] * FZ_TABLE_SIZE)
        return 0;
    tables->count = buf[0];
    for (i = 0; i < tables->count; i++) {
        p = buf + 1 + i * FZ_TABLE_SIZE;
        CopyField(tables->tables[i].id, p, FZ_TABLE_ID_LEN);
        CopyField(tables->tables[i].name, p + FZ_TABLE_ID_LEN, FZ_TABLE_NAME_LEN);
        CopyField(tables->tables[i].seats, p + FZ_TABLE_ID_LEN + FZ_TABLE_NAME_LEN,
                  FZ_TABLE_SEATS_LEN);
    }
    return 1;
}

/* Appends to out at *at, keeping room for the NUL; 0 if it did not fit. */
static int Put(char *out, fn_u16 size, fn_u16 *at, const char *text)
{
    while (*text) {
        if (*at + 1 >= size) {
            out[*at] = '\0';
            return 0;
        }
        out[(*at)++] = *text++;
    }
    out[*at] = '\0';
    return 1;
}

/* A query value: letters, digits and -._~ as they are, space as '+'. */
static int PutValue(char *out, fn_u16 size, fn_u16 *at, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    char esc[4];
    unsigned char c;

    for (; *text; text++) {
        c = (unsigned char)*text;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            esc[0] = (char)c;
            esc[1] = '\0';
        } else if (c == ' ') {
            esc[0] = '+';
            esc[1] = '\0';
        } else {
            esc[0] = '%';
            esc[1] = hex[c >> 4];
            esc[2] = hex[c & 15];
            esc[3] = '\0';
        }
        if (!Put(out, size, at, esc))
            return 0;
    }
    return 1;
}

static int HasScheme(const char *s)
{
    for (; *s && *s != '/' && *s != '?'; s++)
        if (s[0] == ':' && s[1] == '/' && s[2] == '/')
            return 1;
    return 0;
}

int fz_build_url(char *out, fn_u16 size, const char *server, const char *path,
                 const char *table, const char *player)
{
    fn_u16 at = 0, n;

    if (size == 0)
        return 0;
    out[0] = '\0';
    if (!Put(out, size, &at, "N1:"))
        return 0;
    if (!HasScheme(server) && !Put(out, size, &at, "https://"))
        return 0;
    if (!Put(out, size, &at, server))
        return 0;
    n = Len(server);
    if ((n == 0 || server[n - 1] != '/') && !Put(out, size, &at, "/"))
        return 0;
    if (!Put(out, size, &at, path) || !Put(out, size, &at, "?"))
        return 0;
    if (table != 0 && table[0]) {
        if (!Put(out, size, &at, "table=") || !PutValue(out, size, &at, table) ||
            !Put(out, size, &at, "&"))
            return 0;
    }
    if (player != 0 && player[0]) {
        if (!Put(out, size, &at, "player=") || !PutValue(out, size, &at, player) ||
            !Put(out, size, &at, "&"))
            return 0;
    }
    return Put(out, size, &at, "bin=1&be=1");
}

int fz_split_server(char *server, char *table, fn_u16 tableSize)
{
    static const char key[] = "table=";
    fn_u16 i, k, at;

    for (i = 0; server[i] && server[i] != '?'; i++)
        ;
    if (server[i] != '?')
        return 0;
    for (k = i + 1; server[k]; k++) {
        if ((k == i + 1 || server[k - 1] == '&') && server[k] == 't') {
            fn_u16 j = 0;
            while (key[j] && server[k + j] == key[j])
                j++;
            if (key[j] == '\0') {
                k += j;
                for (at = 0; server[k] && server[k] != '&' && at + 1 < tableSize; k++)
                    table[at++] = server[k];
                if (tableSize > 0)
                    table[at] = '\0';
                server[i] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

fn_u16 fz_clean_name(char *name)
{
    fn_u16 in, out = 0;
    char c;

    for (in = 0; name[in] && out < FZ_NAME_LEN - 1; in++) {
        c = name[in];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            name[out++] = c;
        else if (c == ' ' && out > 0 && name[out - 1] != ' ')
            name[out++] = ' ';
    }
    while (out > 0 && name[out - 1] == ' ')
        out--;
    name[out] = '\0';
    return out;
}

const char *fz_display_name(const FzPlayer *player)
{
    const char *n = player->name;

    if (n[0] >= '0' && n[0] <= '9' && n[1] != '\0')
        return n + 1;
    return n;
}

fz_s16 fz_total(const FzPlayer *player)
{
    fz_s16 total = 0;
    int slot;

    for (slot = FZ_UPPER_TOTAL; slot < FZ_TOTAL; slot++)
        if (player->scores[slot] > 0)
            total += player->scores[slot];
    return total;
}

int fz_selectable(int row)
{
    return row >= 0 && row < FZ_CHOICES && row != FZ_UPPER_TOTAL && row != FZ_BONUS;
}

int fz_best_row(const FzGame *game)
{
    int row, best = -1;

    for (row = 0; row < FZ_CHOICES; row++)
        if (fz_selectable(row) && game->validScores[row] >= 0 &&
            (best < 0 || game->validScores[row] > game->validScores[best]))
            best = row;
    return best;
}

const char *fz_short_label(int slot)
{
    static const char *const labels[FZ_SLOTS] = {
        "1s", "2s", "3s", "4s", "5s", "6s", "Up", "Bn",
        "3K", "4K", "FH", "SR", "LR", "Ch", "FZ", "Tot"
    };

    return slot >= 0 && slot < FZ_SLOTS ? labels[slot] : "";
}

const char *fz_label(int slot)
{
    static const char *const labels[FZ_SLOTS] = {
        "Ones", "Twos", "Threes", "Fours", "Fives", "Sixes", "Upper", "Bonus",
        "3 Kind", "4 Kind", "House", "Sm Run", "Lg Run", "Chance", "Fujitzee", "Total"
    };

    return slot >= 0 && slot < FZ_SLOTS ? labels[slot] : "";
}
