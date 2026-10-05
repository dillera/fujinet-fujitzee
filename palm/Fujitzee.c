/* FN Fujitzee for Palm OS.
 *
 * A client for the Fujitzee server, at the same tables as this repository's
 * Atari, Apple II, CoCo, Adam and cartridge clients. It asks for the
 * server's packed binary state (bin=1, with be=1 for big-endian numbers)
 * through FujiNet's N1: device as a plain HTTP GET, polls it about once a
 * second, and sends roll/score/ready moves; every rule is the server's.
 * See fzwire.h.
 */
#include <PalmOS.h>

#include "fnnet.h"
#include "fzwire.h"
#include "FujitzeeRsc.h"

#define CREATOR APP_CREATOR
#define PREFS_ID 1
#define PREFS_VERSION 1

#define DEFAULT_SERVER "https://fujitzee.carr-designs.com/"
#define DEFAULT_TABLE  "ai2"
#define SERVER_LEN 64
#define URL_LEN 160

#define NET_UNIT 1
#define FIRST_STATUS_TICKS (SysTicksPerSecond() * 15)
#define STALL_TICKS (SysTicksPerSecond() * 10)
#define READ_POLL_TICKS (SysTicksPerSecond() / 20)
#define POLL_GAP_TICKS (SysTicksPerSecond())
/* On our own turn nothing moves but the clock; a slower poll still sees a
 * force-score, and keeps FujiBus traffic on the cradle line. */
#define TURN_POLL_TICKS (SysTicksPerSecond() * 5)
#define TICK_TICKS (SysTicksPerSecond() / 4)
/* Polls in a row that may fail before the table gives up. */
#define MAX_FAILURES 4

#define fzErrTimeout  (appErrorClass | 0x10)
#define fzErrBadReply (appErrorClass | 0x11)
#define fzErrTooLong  (appErrorClass | 0x12)
#define fzErrNoTable  (appErrorClass | 0x13)

/* Posted once a form is drawn, to start its first fetch. */
#define fetchEvent firstUserEvent

typedef struct {
    char name[FZ_NAME_LEN];
    char table[FZ_TABLE_ID_LEN];
    char server[SERVER_LEN];
} FzPrefs;

/* Screen layout of the table form. */
#define HEADER_H 11
#define CARD_Y   12
#define ROW_H    11
#define CARD_ROWS 8
#define COL_W    80
#define DICE_Y   104
#define DIE      20
#define DIE_STEP 24
#define DIE_X    3
#define ROLL_X   124
#define ROLL_W   34
#define STATUS_Y 127
#define BAR_Y    139
#define BAR_H    21

static FzPrefs gPrefs;
static FzGame gGame;
static FzTables gTables;
static fn_u8 gReply[FZ_STATE_MAX + 4];
static char gUrl[URL_LEN];
static char gStatus[48];
static WinHandle gOffscreen;

static Boolean gSeated;         /* at the table form, polling */
static Boolean gHaveGame;       /* gGame holds a state */
static Boolean gMyTurn;         /* waiting for this player's move */
static UInt32 gNextPoll;
static UInt32 gTurnEnd;         /* tick the server's move clock runs out */
static UInt16 gFailures;
static char gNotice[48];        /* trouble shown on the status line */
static char gMask[FZ_DICE_LEN]; /* the staged roll: '1' re-rolls, '0' holds */
static Int16 gRow;              /* the picked scorecard row, or -1 */
static Int16 gView;             /* whose card is shown */
static Boolean gViewPinned;     /* paged by hand; kept until the turn moves */
static UInt8 gLastRolls;
static UInt8 gLastRound;
static Int8 gLastActive;

/* ---- Errors ---------------------------------------------------------- */

static void ErrorText(Err err, UInt8 ndevErr, char *out)
{
    if (err == fnErrNoReply)
        StrCopy(out, "No answer from FujiNet. Is the cradle cabled to it?");
    else if (err == fnErrRefused)
        StrCopy(out, "FujiNet refused the request.");
    else if (err == fnErrTooBig)
        StrCopy(out, "The server address is too long.");
    else if (err == fzErrTimeout)
        StrCopy(out, "The server stopped sending.");
    else if (err == fzErrBadReply || err == fzErrTooLong)
        StrCopy(out, "The server's reply was not a Fujitzee state. Check the server address.");
    else if (err == fzErrNoTable)
        StrCopy(out, "The server has no such table.");
    else if (err == errNone && ndevErr != 0)
        StrPrintF(out, "FujiNet network error %d. Is its WiFi up, and the server address right?",
                  ndevErr);
    else
        StrPrintF(out, "Error %x.", err);
}

static void ShowError(const char *what, Err err, UInt8 ndevErr)
{
    char detail[96];

    ErrorText(err, ndevErr, detail);
    FrmCustomAlert(MessageAlert, what, "\n", detail);
}

/* ---- HTTP through N1: ------------------------------------------------ */

/* One GET of gUrl into gReply. *ndevErr is FujiNet's status code when the
 * request ended badly (an HTTP error, or no network). */
static Err HttpGet(UInt16 *len, UInt8 *ndevErr)
{
    FnNetStatus status;
    UInt32 last;
    UInt16 want, got;
    Err err;

    *len = 0;
    *ndevErr = 0;
    err = FnNetOpen(NET_UNIT, gUrl, FN_NET_HTTP_GET, FN_NET_TRANS_NONE);
    if (err != errNone)
        return err;
    err = FnNetGetStatusWait(NET_UNIT, &status, FIRST_STATUS_TICKS);
    last = TimGetTicks();
    while (err == errNone) {
        EvtResetAutoOffTimer();
        if (status.avail > 0) {
            want = sizeof(gReply) - *len;
            if (want == 0) {
                err = fzErrTooLong;
                break;
            }
            if (want > status.avail)
                want = status.avail;
            if (want > FN_NET_CHUNK)
                want = FN_NET_CHUNK;
            if ((err = FnNetRead(NET_UNIT, gReply + *len, want, &got)) != errNone)
                break;
            *len += got;
            last = TimGetTicks();
        } else if (!status.connected) {
            if (status.error != FN_NET_OK && status.error != FN_NET_EOF)
                *ndevErr = status.error;
            break;
        } else if (TimGetTicks() - last > STALL_TICKS) {
            err = fzErrTimeout;
            break;
        } else {
            SysTaskDelay(READ_POLL_TICKS);
        }
        err = FnNetGetStatus(NET_UNIT, &status);
    }
    FnNetClose(NET_UNIT);
    return err;
}

/* A GET of a game call (state, ready, roll/..., score/N, leave) into gGame.
 * A READ is sent once (see fnnet.h), so a lost one spoils the whole reply;
 * that is tried once more. */
static Err GameCall(const char *path, UInt8 *ndevErr)
{
    UInt16 len, attempt;
    Err err = errNone;

    *ndevErr = 0;
    if (!fz_build_url(gUrl, sizeof(gUrl), gPrefs.server, path, gPrefs.table, gPrefs.name))
        return fnErrTooBig;
    for (attempt = 0; attempt < 2; attempt++) {
        err = HttpGet(&len, ndevErr);
        if (err != fnErrNoReply)
            break;
    }
    if (err != errNone || *ndevErr != 0)
        return err;
    if (len == 0)
        return fzErrNoTable;
    if (!fz_parse_game(gReply, len, &gGame))
        return fzErrBadReply;
    gHaveGame = true;
    return errNone;
}

static Err FetchTables(UInt8 *ndevErr)
{
    UInt16 len, attempt;
    Err err = errNone;

    *ndevErr = 0;
    if (!fz_build_url(gUrl, sizeof(gUrl), gPrefs.server, "tables", NULL, NULL))
        return fnErrTooBig;
    for (attempt = 0; attempt < 2; attempt++) {
        err = HttpGet(&len, ndevErr);
        if (err != fnErrNoReply)
            break;
    }
    if (err != errNone || *ndevErr != 0)
        return err;
    if (!fz_parse_tables(gReply, len, &gTables))
        return fzErrBadReply;
    return errNone;
}

/* ---- Drawing helpers -------------------------------------------------- */

/* Draws text cut to fit width, ending in "..." when it was cut. */
static void DrawFit(const char *text, Coord x, Coord y, Int16 width, Boolean inverted)
{
    Int16 w = width, len = StrLen(text);
    Boolean fits;
    char buf[96];

    FntCharsInWidth(text, &w, &len, &fits);
    if (!fits && len > 0) {
        w = width - FntCharsWidth("...", 3);
        len = StrLen(text);
        FntCharsInWidth(text, &w, &len, &fits);
        if (len > (Int16)sizeof(buf) - 4)
            len = sizeof(buf) - 4;
        MemMove(buf, text, len);
        StrCopy(buf + len, "...");
        text = buf;
        len += 3;
    }
    if (inverted)
        WinDrawInvertedChars(text, len, x, y);
    else
        WinDrawChars(text, len, x, y);
}

static void DrawRight(const char *text, Coord right, Coord y)
{
    Int16 len = StrLen(text);

    WinDrawChars(text, len, right - FntCharsWidth(text, len), y);
}

static void DrawCentered(const char *text, Coord left, Coord width, Coord y)
{
    Int16 len = StrLen(text);

    WinDrawChars(text, len, left + (width - FntCharsWidth(text, len)) / 2, y);
}

/* "ai clyd" -> "Ai Clyd": the server lowercases everything. */
static void TitleCase(const char *in, char *out, UInt16 size)
{
    UInt16 i;
    Boolean start = true;

    for (i = 0; in[i] && i + 1 < size; i++) {
        char c = in[i];
        if (start && c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        start = c == ' ' || c == '-';
        out[i] = c;
    }
    out[i] = '\0';
}

/* The server's prompt with its first letter capitalized. */
static void Sentence(const char *in, char *out, UInt16 size)
{
    StrNCopy(out, in, size - 1);
    out[size - 1] = '\0';
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = out[0] - 'a' + 'A';
}

static Boolean InRect(const RectangleType *r, Coord x, Coord y)
{
    return x >= r->topLeft.x && x < r->topLeft.x + r->extent.x &&
           y >= r->topLeft.y && y < r->topLeft.y + r->extent.y;
}

static void SetRect(RectangleType *r, Coord x, Coord y, Coord w, Coord h)
{
    r->topLeft.x = x;
    r->topLeft.y = y;
    r->extent.x = w;
    r->extent.y = h;
}

/* Tracks the pen in a button as a control would; true if let go inside. */
static Boolean TrackPen(const RectangleType *r)
{
    Coord x, y;
    Boolean down = true, inside = true, wasInside = true;

    WinInvertRectangle(r, 3);
    while (down) {
        EvtGetPen(&x, &y, &down);
        inside = InRect(r, x, y);
        if (inside != wasInside) {
            WinInvertRectangle(r, 3);
            wasInside = inside;
        }
    }
    if (wasInside)
        WinInvertRectangle(r, 3);
    return inside;
}

static void DrawButton(const RectangleType *r, const char *label)
{
    WinEraseRectangle(r, 3);
    WinDrawRectangleFrame(roundFrame, r);
    FntSetFont(boldFont);
    DrawCentered(label, r->topLeft.x, r->extent.x, r->topLeft.y + (r->extent.y - 11) / 2);
    FntSetFont(stdFont);
}

/* ---- Game state ------------------------------------------------------- */

static Boolean InPlay(void)
{
    return gHaveGame && gGame.round > 0 && gGame.round != FZ_ROUND_OVER;
}

static Boolean HaveDice(void)
{
    return StrLen(gGame.dice) == 5;
}

static Int16 SecondsLeft(void)
{
    Int32 left;

    if (!gMyTurn)
        return 0;
    left = (Int32)(gTurnEnd - TimGetTicks());
    if (left <= 0)
        return 0;
    return (Int16)((left + SysTicksPerSecond() - 1) / SysTicksPerSecond());
}

static Boolean MaskRollsAny(void)
{
    UInt16 i;

    for (i = 0; i < 5; i++)
        if (gMask[i] == '1')
            return true;
    return false;
}

/* The mask to stage: the wire's echo of the last roll keeps held dice held. */
static void RestageMask(void)
{
    if (StrLen(gGame.keepRoll) == 5)
        StrCopy(gMask, gGame.keepRoll);
    else
        StrCopy(gMask, "11111");
}

/* Called after every state: works out the turn, roll and view edges. */
static void AfterState(void)
{
    Boolean wasTurn = gMyTurn;

    gMyTurn = InPlay() && !gGame.viewing && gGame.activePlayer == 0 && gGame.moveTime > 0;
    if (gMyTurn) {
        gTurnEnd = TimGetTicks() + (UInt32)gGame.moveTime * SysTicksPerSecond();
        /* A new round with the turn still ours (a table to ourselves) is a
         * new turn too. */
        if (!wasTurn || gGame.round != gLastRound) {
            RestageMask();
            gRow = gGame.rollsLeft == 0 ? fz_best_row(&gGame) : -1;
            SndPlaySystemSound(sndConfirmation);
        } else if (gGame.rollsLeft < gLastRolls) {
            RestageMask();
            if (gGame.rollsLeft == 0 && gRow < 0)
                gRow = fz_best_row(&gGame);
        }
        if (gRow >= 0 && gGame.validScores[gRow] < 0)
            gRow = fz_best_row(&gGame);
        gView = 0;
        gViewPinned = false;
        gNextPoll = TimGetTicks() + TURN_POLL_TICKS;
    } else {
        gRow = -1;
        if (gGame.activePlayer != gLastActive)
            gViewPinned = false;
        if (!gViewPinned)
            gView = gGame.activePlayer >= 0 ? gGame.activePlayer : 0;
        gNextPoll = TimGetTicks() + POLL_GAP_TICKS;
    }
    if (gView >= gGame.playerCount)
        gView = 0;
    gLastRolls = gGame.rollsLeft;
    gLastRound = gGame.round;
    gLastActive = gGame.activePlayer;
}

/* ---- The table -------------------------------------------------------- */

static void CellRect(Int16 slot, RectangleType *r)
{
    SetRect(r, (slot / CARD_ROWS) * COL_W, CARD_Y + (slot % CARD_ROWS) * ROW_H, COL_W - 1, ROW_H);
}

/* Whether slot on the shown card is one this player may take now. */
static Boolean Offered(Int16 slot)
{
    return gMyTurn && gView == 0 && fz_selectable(slot) && gGame.validScores[slot] >= 0 &&
           gGame.players[0].scores[slot] == FZ_EMPTY;
}

static void DrawCell(Int16 slot)
{
    const FzPlayer *p = &gGame.players[gView];
    RectangleType r;
    char value[8];
    fz_s16 score = p->scores[slot];

    CellRect(slot, &r);
    WinEraseRectangle(&r, 0);
    FntSetFont(slot == FZ_TOTAL ? boldFont : stdFont);
    WinDrawChars(fz_label(slot), StrLen(fz_label(slot)), r.topLeft.x + 2, r.topLeft.y);

    value[0] = '\0';
    if (slot == FZ_TOTAL) {
        StrIToA(value, gGame.round == FZ_ROUND_OVER && score >= 0 ? score : fz_total(p));
    } else if (score >= 0) {
        StrIToA(value, score);
    } else if (Offered(slot)) {
        /* What the row would earn: a dotted underline marks it as an offer. */
        StrIToA(value, gGame.validScores[slot]);
        WinDrawGrayLine(r.topLeft.x + r.extent.x - 18, r.topLeft.y + ROW_H - 1,
                        r.topLeft.x + r.extent.x - 3, r.topLeft.y + ROW_H - 1);
    }
    if (value[0])
        DrawRight(value, r.topLeft.x + r.extent.x - 3, r.topLeft.y);
    FntSetFont(stdFont);
    if (slot == gRow && Offered(slot))
        WinInvertRectangle(&r, 0);
}

static void DrawCard(void)
{
    Int16 slot;

    for (slot = 0; slot < FZ_SLOTS; slot++)
        DrawCell(slot);
    WinDrawLine(COL_W - 1, CARD_Y, COL_W - 1, CARD_Y + CARD_ROWS * ROW_H - 1);
    WinDrawGrayLine(0, CARD_Y + 6 * ROW_H, COL_W - 2, CARD_Y + 6 * ROW_H);
    WinDrawGrayLine(COL_W, CARD_Y + 7 * ROW_H, 159, CARD_Y + 7 * ROW_H);
}

static void DrawHeader(void)
{
    char text[32], name[FZ_NAME_LEN];
    RectangleType r;
    Int16 left;
    Boolean active = gGame.activePlayer == gView;

    SetRect(&r, 0, 0, 160, HEADER_H);
    WinEraseRectangle(&r, 0);
    FntSetFont(boldFont);
    if (gGame.playerCount > 1) {
        WinDrawChars("<", 1, 1, -1);
        WinDrawChars(">", 1, 153, -1);
    }
    TitleCase(fz_display_name(&gGame.players[gView]), name, sizeof(name));
    if (active) {
        SetRect(&r, 9, 0, 62, HEADER_H - 1);
        WinDrawRectangle(&r, 2);
        DrawFit(name, 11, -1, 58, true);
    } else {
        DrawFit(name, 11, -1, 58, false);
    }
    FntSetFont(stdFont);
    StrPrintF(text, "Round %u/%u", gGame.round, FZ_ROUNDS);
    WinDrawChars(text, StrLen(text), 74, -1);
    left = SecondsLeft();
    if (left > 0) {
        StrIToA(text, left);
        FntSetFont(boldFont);
        DrawRight(text, 150, -1);
        FntSetFont(stdFont);
    }
    WinDrawLine(0, HEADER_H, 159, HEADER_H);
}

/* A die face: pips are bits of a 3x3 grid, top-left first. */
static void DrawDie(Int16 i)
{
    static const UInt16 faces[7] = { 0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16D };
    RectangleType r, pip;
    Coord x = DIE_X + i * DIE_STEP;
    Boolean held, have = HaveDice();
    UInt16 face, bit;
    Int16 v;

    SetRect(&r, x, DICE_Y, DIE, DIE);
    WinEraseRectangle(&r, 0);
    v = have ? gGame.dice[i] - '0' : 0;
    if (v < 0 || v > 6)
        v = 0;
    /* Held dice are black. On our turn that is the staged mask; for anyone
     * else, the wire's echo of what they last kept. */
    held = have && (gMyTurn ? gMask[i] == '0' : (StrLen(gGame.keepRoll) == 5 &&
                                                 gGame.keepRoll[i] == '0' && gGame.rollsLeft < 2));
    if (held)
        WinDrawRectangle(&r, 3);
    else
        WinDrawRectangleFrame(roundFrame, &r);
    face = faces[v];
    for (bit = 0; bit < 9; bit++) {
        if (!(face & (0x100 >> bit)))
            continue;
        SetRect(&pip, x + 3 + (bit % 3) * 6, DICE_Y + 3 + (bit / 3) * 6, 3, 3);
        if (held)
            WinEraseRectangle(&pip, 0);
        else
            WinDrawRectangle(&pip, 0);
    }
    if (!have) {
        FntSetFont(stdFont);
        DrawCentered("?", x, DIE, DICE_Y + 4);
    }
}

static void RollRect(RectangleType *r)
{
    SetRect(r, ROLL_X, DICE_Y, ROLL_W, DIE);
}

static void DrawRoll(void)
{
    RectangleType r;
    char text[12];

    RollRect(&r);
    WinEraseRectangle(&r, 3);
    StrPrintF(text, "%u left", gGame.rollsLeft);
    if (!gMyTurn || gGame.rollsLeft == 0) {
        FntSetFont(stdFont);
        DrawCentered(gMyTurn ? "Score" : "Rolls", r.topLeft.x, r.extent.x, r.topLeft.y - 1);
        DrawCentered(text, r.topLeft.x, r.extent.x, r.topLeft.y + 9);
        return;
    }
    WinDrawRectangleFrame(roundFrame, &r);
    FntSetFont(boldFont);
    DrawCentered("Roll", r.topLeft.x, r.extent.x, r.topLeft.y - 1);
    FntSetFont(stdFont);
    DrawCentered(text, r.topLeft.x, r.extent.x, r.topLeft.y + 9);
}

static void DrawDice(void)
{
    Int16 i;

    for (i = 0; i < 5; i++)
        DrawDie(i);
    DrawRoll();
}

static void DrawStatus(void)
{
    char text[FZ_PROMPT_LEN + 8];
    RectangleType r;

    SetRect(&r, 0, STATUS_Y, 160, 11);
    WinEraseRectangle(&r, 0);
    if (gNotice[0])
        StrCopy(text, gNotice);
    else
        Sentence(gGame.prompt, text, sizeof(text));
    FntSetFont(stdFont);
    DrawFit(text, 2, STATUS_Y, 156, false);
}

/* The bottom bar's buttons: one wide on the left, one on the right. */
static void LeftButtonRect(RectangleType *r)
{
    SetRect(r, 2, BAR_Y + 3, 104, BAR_H - 5);
}

static void RightButtonRect(RectangleType *r)
{
    SetRect(r, 112, BAR_Y + 3, 46, BAR_H - 5);
}

static Boolean MeReady(void)
{
    return gGame.playerCount > 0 && gGame.players[0].scores[0] == 1;
}

/* What the left button does now, and its label; false if there is none. */
static Boolean LeftAction(char *label)
{
    if (!gHaveGame)
        return false;
    if (gGame.round == 0 || gGame.round == FZ_ROUND_OVER) {
        if (gGame.viewing)
            return false;
        StrCopy(label, gGame.round == 0 && MeReady() ? "Not Ready" : "Ready");
        return true;
    }
    if (gMyTurn && gRow >= 0) {
        StrPrintF(label, "Take %s %d", fz_label(gRow), gGame.validScores[gRow]);
        return true;
    }
    if (!gMyTurn) {
        StrCopy(label, "Scores");
        return true;
    }
    return false;
}

static void DrawBar(void)
{
    RectangleType r;
    char label[24];

    SetRect(&r, 0, BAR_Y, 160, BAR_H);
    WinEraseRectangle(&r, 0);
    WinDrawLine(0, BAR_Y, 159, BAR_Y);
    if (LeftAction(label)) {
        LeftButtonRect(&r);
        DrawButton(&r, label);
    } else if (gMyTurn) {
        FntSetFont(stdFont);
        WinDrawChars("Hold dice and Roll, or pick a row", 33, 2, BAR_Y + 5);
        return;
    }
    if (!gMyTurn) {
        RightButtonRect(&r);
        DrawButton(&r, "Leave");
    }
}

/* Round 0 and game over: the seats and how each stands. */
static void DrawRoom(void)
{
    char text[32], name[FZ_NAME_LEN];
    Int16 i, y, rowH;
    fz_s16 s;

    FntSetFont(boldFont);
    TitleCase(gGame.serverName, text, sizeof(text));
    DrawCentered(text, 0, 160, 0);
    FntSetFont(stdFont);
    WinDrawLine(0, HEADER_H, 159, HEADER_H);
    DrawCentered(gGame.round == 0 ? "Waiting room" : "Final scores", 0, 160, HEADER_H + 2);
    /* The list fits between the heading and the status line. */
    rowH = gGame.playerCount > 0 ? (STATUS_Y - HEADER_H - 16) / gGame.playerCount : 11;
    if (rowH > 11)
        rowH = 11;
    for (i = 0; i < gGame.playerCount; i++) {
        y = HEADER_H + 15 + i * rowH;
        TitleCase(fz_display_name(&gGame.players[i]), name, sizeof(name));
        FntSetFont(i == 0 ? boldFont : stdFont);
        WinDrawChars(name, StrLen(name), 20, y);
        FntSetFont(stdFont);
        s = gGame.players[i].scores[0];
        if (gGame.round == 0)
            StrCopy(text, s == FZ_SPECTATOR ? "Watching" : s == 1 ? "Ready" : "Not ready");
        else
            StrIToA(text, gGame.players[i].scores[FZ_TOTAL] >= 0 ?
                    gGame.players[i].scores[FZ_TOTAL] : fz_total(&gGame.players[i]));
        DrawRight(text, 140, y);
    }
}

static void DrawTable(void)
{
    RectangleType r;
    WinHandle old = NULL;

    SetRect(&r, 0, 0, 160, 160);
    if (gOffscreen)
        old = WinSetDrawWindow(gOffscreen);
    WinEraseRectangle(&r, 0);

    if (!gHaveGame) {
        FntSetFont(boldFont);
        DrawCentered("FN Fujitzee", 0, 160, 40);
        FntSetFont(stdFont);
        DrawFit(gNotice[0] ? gNotice : "Taking a seat...", 4, 60, 152, false);
    } else if (!InPlay()) {
        DrawRoom();
        DrawStatus();
        DrawBar();
    } else {
        DrawHeader();
        DrawCard();
        DrawDice();
        DrawStatus();
        DrawBar();
    }

    if (gOffscreen) {
        WinSetDrawWindow(old);
        WinCopyRectangle(gOffscreen, NULL, &r, 0, 0, winPaint);
    }
}

/* ---- Play ------------------------------------------------------------- */

static void LeaveTable(void);

static void PollFailed(Err err, UInt8 ndevErr)
{
    char detail[96];

    gFailures++;
    if (gFailures >= MAX_FAILURES) {
        LeaveTable();
        ShowError("Lost the table.", err, ndevErr);
        FrmGotoForm(SetupForm);
        return;
    }
    ErrorText(err, ndevErr, detail);
    StrPrintF(gNotice, "Retrying (%u)... ", gFailures);
    StrNCat(gNotice, detail, sizeof(gNotice) - 1);
    gNextPoll = TimGetTicks() + POLL_GAP_TICKS * 2;
    DrawTable();
}

static void Poll(void)
{
    UInt8 ndevErr;
    Err err;

    err = GameCall("state", &ndevErr);
    if (err == fzErrNoTable && !gHaveGame) {
        /* A table that does not exist is not worth retrying. */
        gSeated = false;
        ShowError("Could not sit down.", err, 0);
        FrmGotoForm(SetupForm);
        return;
    }
    if (err != errNone || ndevErr != 0) {
        PollFailed(err, ndevErr);
        return;
    }
    gFailures = 0;
    gNotice[0] = '\0';
    AfterState();
    DrawTable();
}

/* Sends a move; its reply is the next state. */
static void PlayMove(const char *path)
{
    UInt8 ndevErr;
    Err err;

    StrCopy(gNotice, "Sending...");
    DrawStatus();
    gNotice[0] = '\0';
    err = GameCall(path, &ndevErr);
    if (err != errNone || ndevErr != 0) {
        StrCopy(gNotice, "Move not sent. Try it again.");
        DrawTable();
        return;
    }
    AfterState();
    DrawTable();
}

static void Roll(void)
{
    char path[12];

    if (!gMyTurn || gGame.rollsLeft == 0 || !HaveDice())
        return;
    if (!MaskRollsAny()) {
        /* Every die is held: nothing to roll. */
        SndPlaySystemSound(sndError);
        return;
    }
    StrCopy(path, "roll/");
    StrCat(path, gMask);
    PlayMove(path);
}

static void TakeRow(Int16 row)
{
    char path[12];

    if (!Offered(row))
        return;
    StrCopy(path, "score/");
    StrIToA(path + 6, row);
    gRow = -1;
    PlayMove(path);
}

static void ToggleReady(void)
{
    if (gGame.viewing)
        return;
    PlayMove("ready");
}

static void ToggleHold(Int16 i)
{
    if (!gMyTurn || gGame.rollsLeft == 0 || !HaveDice() || i < 0 || i > 4)
        return;
    gMask[i] = gMask[i] == '1' ? '0' : '1';
    SndPlaySystemSound(sndClick);
    DrawTable();
}

/* Gives the seat back to the server, best effort. */
static void LeaveTable(void)
{
    UInt8 ndevErr;

    if (!gSeated)
        return;
    gSeated = false;
    gMyTurn = false;
    if (gFailures < MAX_FAILURES)
        GameCall("leave", &ndevErr);
}

static void ShowScores(void)
{
    char text[200], line[24], name[FZ_NAME_LEN];
    Int16 i;

    if (!gHaveGame || gGame.playerCount == 0) {
        FrmCustomAlert(ScoresAlert, "Nobody is seated yet.", "", "");
        return;
    }
    text[0] = '\0';
    for (i = 0; i < gGame.playerCount && i < FZ_PLAYER_MAX; i++) {
        TitleCase(fz_display_name(&gGame.players[i]), name, sizeof(name));
        StrPrintF(line, "%s: %d\n", name, fz_total(&gGame.players[i]));
        if (StrLen(text) + StrLen(line) < sizeof(text))
            StrCat(text, line);
    }
    FrmCustomAlert(ScoresAlert, text, "", "");
}

static void PageView(Int16 step)
{
    if (!InPlay() || gGame.playerCount < 2)
        return;
    gView = (gView + step + gGame.playerCount) % gGame.playerCount;
    gViewPinned = true;
    DrawTable();
}

static void MoveRow(Int16 step)
{
    Int16 row = gRow, n;

    if (!gMyTurn)
        return;
    for (n = 0; n < FZ_CHOICES; n++) {
        row = row < 0 ? (step > 0 ? 0 : FZ_CHOICES - 1) : (row + step + FZ_CHOICES) % FZ_CHOICES;
        if (Offered(row)) {
            gRow = row;
            DrawTable();
            return;
        }
    }
}

static Boolean TablePen(Coord x, Coord y)
{
    RectangleType r;
    Int16 i;
    char label[24];

    if (!gHaveGame)
        return false;

    if (InPlay()) {
        if (y < HEADER_H) {
            if (x < 40)
                PageView(-1);
            else if (x >= 120)
                PageView(1);
            return true;
        }
        for (i = 0; i < 5; i++) {
            SetRect(&r, DIE_X + i * DIE_STEP, DICE_Y, DIE, DIE);
            if (InRect(&r, x, y)) {
                ToggleHold(i);
                return true;
            }
        }
        RollRect(&r);
        if (InRect(&r, x, y)) {
            if (gMyTurn && gGame.rollsLeft > 0 && TrackPen(&r))
                Roll();
            return true;
        }
        for (i = 0; i < FZ_SLOTS; i++) {
            CellRect(i, &r);
            if (InRect(&r, x, y)) {
                if (Offered(i)) {
                    if (gRow == i)
                        TakeRow(i);
                    else {
                        gRow = i;
                        SndPlaySystemSound(sndClick);
                        DrawTable();
                    }
                }
                return true;
            }
        }
    }

    LeftButtonRect(&r);
    if (InRect(&r, x, y) && LeftAction(label)) {
        if (TrackPen(&r)) {
            if (gGame.round == 0 || gGame.round == FZ_ROUND_OVER)
                ToggleReady();
            else if (gMyTurn)
                TakeRow(gRow);
            else
                ShowScores();
        }
        return true;
    }
    RightButtonRect(&r);
    if (InRect(&r, x, y) && !gMyTurn) {
        if (TrackPen(&r)) {
            LeaveTable();
            FrmGotoForm(SetupForm);
        }
        return true;
    }
    return false;
}

/* Scroll keys pick a row (or page cards between turns); Date Book rolls,
 * the other app buttons take the picked row. Graffiti 1-5 hold, r rolls. */
static Boolean TableKey(WChar c)
{
    if (c == vchrPageUp || c == vchrPageDown) {
        if (gMyTurn)
            MoveRow(c == vchrPageUp ? -1 : 1);
        else
            PageView(c == vchrPageUp ? -1 : 1);
        return true;
    }
    if (c == vchrHard1) {
        Roll();
        return true;
    }
    if (c >= vchrHard2 && c <= vchrHard4) {
        if (gMyTurn && gRow >= 0)
            TakeRow(gRow);
        else if (gHaveGame && (gGame.round == 0 || gGame.round == FZ_ROUND_OVER))
            ToggleReady();
        return true;
    }
    if (c >= '1' && c <= '5') {
        ToggleHold(c - '1');
        return true;
    }
    if (c == 'r' || c == 'R') {
        Roll();
        return true;
    }
    return false;
}

static Boolean TableHandleEvent(EventType *event)
{
    switch (event->eType) {
    case frmOpenEvent:
        FrmDrawForm(FrmGetActiveForm());
        gSeated = true;
        gHaveGame = false;
        MemSet(&gGame, sizeof(gGame), 0);
        gMyTurn = false;
        gFailures = 0;
        gRow = -1;
        gView = 0;
        gViewPinned = false;
        gLastRolls = 0;
        gLastRound = 0;
        gLastActive = -1;
        StrCopy(gNotice, "Taking a seat...");
        DrawTable();
        gNotice[0] = '\0';
        gNextPoll = TimGetTicks();
        return true;
    case frmUpdateEvent:
        DrawTable();
        return true;
    case frmCloseEvent:
        LeaveTable();
        return false;
    case penDownEvent:
        return TablePen(event->screenX, event->screenY);
    case keyDownEvent:
        return TableKey(event->data.keyDown.chr);
    case menuEvent:
        switch (event->data.menu.itemID) {
        case TableMenuLeave:
            LeaveTable();
            FrmGotoForm(SetupForm);
            return true;
        case TableMenuScores:
            ShowScores();
            return true;
        case TableMenuHelp:
            FrmAlert(HelpAlert);
            return true;
        case TableMenuAbout:
            FrmAlert(AboutAlert);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

/* Called on every event while seated: polls, and ticks the move clock. */
static void TableIdle(void)
{
    static Int16 lastShown = -1;
    Int16 left;

    if (!gSeated || FrmGetActiveFormID() != TableForm)
        return;
    EvtResetAutoOffTimer();
    if (gMyTurn) {
        left = SecondsLeft();
        if (left != lastShown) {
            lastShown = left;
            DrawHeader();
        }
        if (left == 0) {
            /* The server scores for us when the clock runs out. */
            gMyTurn = false;
            gNextPoll = TimGetTicks();
        }
    }
    if ((Int32)(TimGetTicks() - gNextPoll) >= 0)
        Poll();
}

/* ---- Setup form ------------------------------------------------------- */

static void *GetObject(FormType *form, UInt16 id)
{
    return FrmGetObjectPtr(form, FrmGetObjectIndex(form, id));
}

static void SetFieldText(FormType *form, UInt16 id, const char *text)
{
    FieldType *field = GetObject(form, id);
    MemHandle old = FldGetTextHandle(field);
    MemHandle handle = MemHandleNew(StrLen(text) + 1);

    if (handle == NULL)
        return;
    StrCopy(MemHandleLock(handle), text);
    MemHandleUnlock(handle);
    FldSetTextHandle(field, handle);
    if (old)
        MemHandleFree(old);
}

static void GetFieldText(FormType *form, UInt16 id, char *out, UInt16 size)
{
    const char *text = FldGetTextPtr(GetObject(form, id));

    if (text == NULL)
        text = "";
    StrNCopy(out, text, size - 1);
    out[size - 1] = '\0';
}

static void SetStatus(const char *text)
{
    FormType *form = FrmGetFormPtr(SetupForm);
    FieldType *field;

    StrNCopy(gStatus, text, sizeof(gStatus) - 1);
    gStatus[sizeof(gStatus) - 1] = '\0';
    if (form == NULL)
        return;
    field = GetObject(form, SetupStatusField);
    FldSetTextPtr(field, gStatus);
    FldRecalculateField(field, false);
    if (FrmGetActiveForm() == form && FrmVisible(form))
        FldDrawField(field);
}

static void ReadSetup(FormType *form)
{
    char server[SERVER_LEN];

    GetFieldText(form, SetupNameField, gPrefs.name, sizeof(gPrefs.name));
    fz_clean_name(gPrefs.name);
    GetFieldText(form, SetupTableField, gPrefs.table, sizeof(gPrefs.table));
    GetFieldText(form, SetupServerField, server, sizeof(server));
    if (server[0] == '\0')
        StrCopy(server, DEFAULT_SERVER);
    /* A lobby address carries its table: take it, and show both split. */
    if (fz_split_server(server, gPrefs.table, sizeof(gPrefs.table))) {
        SetFieldText(form, SetupServerField, server);
        SetFieldText(form, SetupTableField, gPrefs.table);
        if (FrmVisible(form)) {
            FldDrawField(GetObject(form, SetupServerField));
            FldDrawField(GetObject(form, SetupTableField));
        }
    }
    StrCopy(gPrefs.server, server);
    PrefSetAppPreferences(CREATOR, PREFS_ID, PREFS_VERSION, &gPrefs, sizeof(gPrefs), true);
}

static void DrawTableRow(Int16 item, RectangleType *bounds, Char **unused)
{
    const FzTable *t;
    char name[FZ_TABLE_NAME_LEN];
    Coord x = bounds->topLeft.x + 1, y = bounds->topLeft.y;
    Int16 right = bounds->topLeft.x + bounds->extent.x - 2, w;

    if (item < 0 || item >= gTables.count)
        return;
    t = &gTables.tables[item];
    TitleCase(t->name, name, sizeof(name));
    w = FntCharsWidth(t->seats, StrLen(t->seats));
    DrawFit(name, x, y, right - w - 6 - x, false);
    WinDrawChars(t->seats, StrLen(t->seats), right - w, y);
}

static void ShowTables(FormType *form)
{
    ListType *list = GetObject(form, SetupList);
    RectangleType r;
    Int16 i, sel = noListSelection;

    for (i = 0; i < gTables.count; i++)
        if (StrCaselessCompare(gTables.tables[i].id, gPrefs.table) == 0)
            sel = i;
    LstSetListChoices(list, NULL, gTables.count);
    LstSetDrawFunction(list, DrawTableRow);
    LstSetSelection(list, sel);
    if (FrmGetActiveForm() != form || !FrmVisible(form))
        return;
    FrmGetObjectBounds(form, FrmGetObjectIndex(form, SetupList), &r);
    WinEraseRectangle(&r, 0);
    LstDrawList(list);
}

static void RefreshTables(FormType *form)
{
    char text[48];
    UInt8 ndevErr;
    Err err;

    ReadSetup(form);
    gTables.count = 0;
    ShowTables(form);
    SetStatus("Asking the server...");
    err = FetchTables(&ndevErr);
    ShowTables(form);
    if (err != errNone || ndevErr != 0) {
        SetStatus("No table list. Tap Refresh.");
        ShowError("Could not list the tables.", err, ndevErr);
        return;
    }
    StrPrintF(text, "%u tables. Pick one, Sit Down.", gTables.count);
    SetStatus(text);
}

static void SitDown(FormType *form)
{
    ReadSetup(form);
    SetFieldText(form, SetupNameField, gPrefs.name);
    FldDrawField(GetObject(form, SetupNameField));
    if (gPrefs.name[0] == '\0') {
        FrmCustomAlert(MessageAlert, "Enter a name to play under: ",
                       "letters and digits, up to 8.", "");
        FrmSetFocus(form, FrmGetObjectIndex(form, SetupNameField));
        return;
    }
    if (gPrefs.table[0] == '\0') {
        FrmCustomAlert(MessageAlert, "Pick a table from the list, ",
                       "or type its name.", "");
        return;
    }
    FrmGotoForm(TableForm);
}

static Boolean SetupHandleEvent(EventType *event)
{
    FormType *form = FrmGetActiveForm();
    EventType fetch;

    switch (event->eType) {
    case frmOpenEvent:
        SetFieldText(form, SetupNameField, gPrefs.name);
        SetFieldText(form, SetupServerField, gPrefs.server);
        SetFieldText(form, SetupTableField, gPrefs.table);
        FldSetTextPtr(GetObject(form, SetupStatusField), gStatus);
        ShowTables(form);
        FrmDrawForm(form);
        FrmSetFocus(form, FrmGetObjectIndex(form, SetupNameField));
        if (gTables.count == 0) {
            MemSet(&fetch, sizeof(fetch), 0);
            fetch.eType = fetchEvent;
            EvtAddEventToQueue(&fetch);
        }
        return true;
    case frmUpdateEvent:
        FrmDrawForm(form);
        return true;
    case frmCloseEvent:
        ReadSetup(form);
        return false;
    case fetchEvent:
        RefreshTables(form);
        return true;
    case lstSelectEvent:
        if (event->data.lstSelect.listID == SetupList) {
            Int16 i = event->data.lstSelect.selection;
            if (i >= 0 && i < gTables.count) {
                SetFieldText(form, SetupTableField, gTables.tables[i].id);
                FldDrawField(GetObject(form, SetupTableField));
            }
            return true;
        }
        break;
    case ctlSelectEvent:
        switch (event->data.ctlSelect.controlID) {
        case SetupRefreshButton:
            RefreshTables(form);
            return true;
        case SetupSitButton:
            SitDown(form);
            return true;
        }
        break;
    case menuEvent:
        switch (event->data.menu.itemID) {
        case SetupMenuRefresh:
            RefreshTables(form);
            return true;
        case SetupMenuDefault:
            SetFieldText(form, SetupServerField, DEFAULT_SERVER);
            FldDrawField(GetObject(form, SetupServerField));
            return true;
        case SetupMenuAbout:
            FrmAlert(AboutAlert);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

/* ---- Application ------------------------------------------------------ */

static Boolean AppHandleEvent(EventType *event)
{
    FormType *form;

    if (event->eType != frmLoadEvent)
        return false;
    form = FrmInitForm(event->data.frmLoad.formID);
    FrmSetActiveForm(form);
    if (event->data.frmLoad.formID == TableForm)
        FrmSetEventHandler(form, TableHandleEvent);
    else
        FrmSetEventHandler(form, SetupHandleEvent);
    return true;
}

/* While seated, the app buttons play instead of launching apps. */
static Boolean IsTableHardKey(const EventType *event)
{
    WChar c = event->data.keyDown.chr;

    return gSeated && event->eType == keyDownEvent &&
           (event->data.keyDown.modifiers & commandKeyMask) &&
           ((c >= vchrHard1 && c <= vchrHard4) || c == vchrPageUp || c == vchrPageDown) &&
           FrmGetActiveFormID() == TableForm;
}

static void EventLoop(void)
{
    EventType event;
    Err err;

    do {
        EvtGetEvent(&event, gSeated ? TICK_TICKS : evtWaitForever);
        if (IsTableHardKey(&event)) {
            TableKey(event.data.keyDown.chr);
            TableIdle();
            continue;
        }
        if (SysHandleEvent(&event))
            continue;
        if (MenuHandleEvent(0, &event, &err))
            continue;
        if (!AppHandleEvent(&event))
            FrmDispatchEvent(&event);
        if (event.eType != appStopEvent)
            TableIdle();
    } while (event.eType != appStopEvent);
}

/* The Serial Manager with SrmOpen first shipped in Palm OS 3.3. */
static Boolean HasNewSerialManager(void)
{
    UInt32 value;
    return FtrGet(sysFileCSerialMgr, sysFtrNewSerialPresent, &value) == errNone && value != 0;
}

static void LoadPrefs(void)
{
    UInt16 size = sizeof(gPrefs);

    if (PrefGetAppPreferences(CREATOR, PREFS_ID, &gPrefs, &size, true) != PREFS_VERSION ||
        size != sizeof(gPrefs)) {
        MemSet(&gPrefs, sizeof(gPrefs), 0);
        StrCopy(gPrefs.server, DEFAULT_SERVER);
        StrCopy(gPrefs.table, DEFAULT_TABLE);
    }
    gPrefs.name[FZ_NAME_LEN - 1] = '\0';
    gPrefs.table[FZ_TABLE_ID_LEN - 1] = '\0';
    gPrefs.server[SERVER_LEN - 1] = '\0';
}

static Err AppStart(void)
{
    Err err;

    LoadPrefs();
    gStatus[0] = '\0';
    gOffscreen = WinCreateOffscreenWindow(160, 160, screenFormat, &err);
    return FnOpen();
}

static void AppStop(void)
{
    FrmCloseAllForms(); /* the table form's close gives up the seat */
    LeaveTable();
    FnClose();
    if (gOffscreen)
        WinDeleteWindow(gOffscreen, false);
    gOffscreen = NULL;
}

UInt32 PilotMain(UInt16 cmd, MemPtr cmdPBP, UInt16 launchFlags)
{
    Err err;

    if (cmd != sysAppLaunchCmdNormalLaunch)
        return 0;
    if (!HasNewSerialManager()) {
        FrmAlert(RomIncompatibleAlert);
        return 0;
    }
    err = AppStart();
    if (err != errNone) {
        ShowError("Could not open the cradle port.", err, 0);
        AppStop();
        return 0;
    }
    FrmGotoForm(SetupForm);
    EventLoop();
    AppStop();
    return 0;
}
