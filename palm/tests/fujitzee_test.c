/* Checks fzwire.c against replies saved from the live server
 * (https://fujitzee.carr-designs.com/, table ai2, bin=1&be=1). */
#include <stdio.h>
#include <string.h>

#include "fzwire.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static fn_u16 Load(const char *name, fn_u8 *buf, fn_u16 size)
{
    char path[128];
    FILE *f;
    size_t n;

    sprintf(path, "data/%s", name);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return 0;
    }
    n = fread(buf, 1, size, f);
    fclose(f);
    return (fn_u16)n;
}

static void TestTables(void)
{
    fn_u8 buf[FZ_TABLES_MAX + 8];
    FzTables t;
    fn_u16 len = Load("fz_tables.bin", buf, sizeof(buf));

    CHECK(fz_parse_tables(buf, len, &t));
    CHECK(t.count == 4);
    CHECK(strcmp(t.tables[0].id, "ai4") == 0);
    CHECK(strcmp(t.tables[0].name, "ai room - 4 bots") == 0);
    CHECK(strcmp(t.tables[0].seats, "0 / 6") == 0);
    CHECK(strcmp(t.tables[3].id, "bar") == 0);
    CHECK(strcmp(t.tables[3].name, "the bar") == 0);
    CHECK(!fz_parse_tables(buf, (fn_u16)(len - 1), &t));
}

static void TestReady(void)
{
    fn_u8 buf[FZ_STATE_MAX + 8];
    FzGame g;
    fn_u16 len = Load("fz_state_ready.bin", buf, sizeof(buf));

    CHECK(len == FZ_STATE_HEADER + 3 * FZ_PLAYER_SIZE);
    CHECK(fz_parse_game(buf, len, &g));
    CHECK(g.playerCount == 3);
    CHECK(g.round == 0);
    CHECK(g.activePlayer == -1);
    CHECK(strcmp(g.serverName, "ai room - 2 bots") == 0);
    CHECK(strcmp(g.prompt, "waiting for everyone to ready up") == 0);
    CHECK(strcmp(g.players[0].name, "pldev") == 0);
    CHECK(g.players[0].scores[0] == 0);
    CHECK(g.players[1].scores[0] == 1);
    CHECK(strcmp(fz_display_name(&g.players[1]), "ai clyd") == 0);
    CHECK(strcmp(fz_display_name(&g.players[0]), "pldev") == 0);
    CHECK(!fz_parse_game(buf, (fn_u16)(len + 1), &g));
}

static void TestMyTurn(void)
{
    static const signed char valid[FZ_CHOICES] = { 2, 2, 3, 0, 5, 0, -1, -1, 0, 0, 0, 0, 0, 12, 0 };
    fn_u8 buf[FZ_STATE_MAX + 8];
    FzGame g;
    fn_u16 len = Load("fz_state_myturn.bin", buf, sizeof(buf));
    int i;

    CHECK(fz_parse_game(buf, len, &g));
    CHECK(g.round == 1);
    CHECK(g.rollsLeft == 2);
    CHECK(g.activePlayer == 0);
    CHECK(g.moveTime == 250);
    CHECK(g.viewing == 0);
    CHECK(strcmp(g.dice, "21513") == 0);
    CHECK(strcmp(g.keepRoll, "11111") == 0);
    for (i = 0; i < FZ_CHOICES; i++)
        CHECK(g.validScores[i] == valid[i]);
    CHECK(g.players[0].scores[13] == FZ_EMPTY);
    CHECK(fz_best_row(&g) == 13);
    CHECK(fz_total(&g.players[0]) == 0);
}

static void TestRolled(void)
{
    fn_u8 buf[FZ_STATE_MAX + 8];
    FzGame g;
    fn_u16 len = Load("fz_state_rolled.bin", buf, sizeof(buf));

    CHECK(fz_parse_game(buf, len, &g));
    CHECK(g.rollsLeft == 0);
    CHECK(strcmp(g.dice, "11632") == 0);
    CHECK(g.validScores[5] == 6);
    CHECK(g.validScores[13] == 13);
}

static void TestOther(void)
{
    fn_u8 buf[FZ_STATE_MAX + 8];
    FzGame g;
    fn_u16 len = Load("fz_state_other.bin", buf, sizeof(buf));

    CHECK(fz_parse_game(buf, len, &g));
    CHECK(g.activePlayer == 1);
    CHECK(strcmp(g.prompt, "ai clyd's turn") == 0);
    CHECK(g.players[0].scores[13] == 13);
    CHECK(fz_total(&g.players[0]) == 13);
}

static void TestUrl(void)
{
    char url[160], server[64], table[9];

    CHECK(fz_build_url(url, sizeof(url), "https://fujitzee.carr-designs.com/", "roll/11000",
                       "ai2", "bob smith"));
    CHECK(strcmp(url, "N1:https://fujitzee.carr-designs.com/roll/11000?table=ai2&player=bob+smith&bin=1&be=1") == 0);
    CHECK(fz_build_url(url, sizeof(url), "fujitzee.carr-designs.com", "tables", NULL, NULL));
    CHECK(strcmp(url, "N1:https://fujitzee.carr-designs.com/tables?bin=1&be=1") == 0);
    CHECK(!fz_build_url(url, 20, "https://fujitzee.carr-designs.com/", "state", "ai2", "bob"));

    strcpy(server, "https://fujitzee.carr-designs.com/?table=kit");
    CHECK(fz_split_server(server, table, sizeof(table)));
    CHECK(strcmp(server, "https://fujitzee.carr-designs.com/") == 0);
    CHECK(strcmp(table, "kit") == 0);
}

static void TestHelpers(void)
{
    char name[32];

    strcpy(name, "  Bob!  Smith 42");
    CHECK(fz_clean_name(name) == 8);
    CHECK(strcmp(name, "Bob Smit") == 0);
    CHECK(!fz_selectable(FZ_UPPER_TOTAL));
    CHECK(!fz_selectable(FZ_BONUS));
    CHECK(fz_selectable(14));
    CHECK(!fz_selectable(15));
    CHECK(strcmp(fz_label(14), "Fujitzee") == 0);
}

int main(void)
{
    TestTables();
    TestReady();
    TestMyTurn();
    TestRolled();
    TestOther();
    TestUrl();
    TestHelpers();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("fujitzee_test: all passed\n");
    return 0;
}
