# Fujitzee for Palm OS

FN Fujitzee (creator `ADFZ`) plays at the same tables as the other clients here, from a Palm
in its serial HotSync cradle. The cradle is wired through a null modem to a FujiNet running the
HotSync service (the `fujinet-rs232-s3-palm` build of
[fujinet-firmware](https://github.com/dillera/fujinet-firmware), branch `palm-demo-fozz`), which
shares that line between HotSync and FujiBus. The app talks to the server through FujiNet's `N1:`
device as plain HTTP GETs of the binary state (`bin=1&be=1`), polling about once a second; every
rule is the server's. Needs Palm OS 3.3 or later.

## Playing

Pick a name (up to 8 letters and digits) and a table, then Sit Down. In the waiting room tap
**Ready**; the game starts once everyone is.

The screen is your scorecard: upper rows on the left, lower rows and the running total on the
right, with the dice and the Roll button below. On your turn the dice are already rolled. Tap dice
to hold them (held dice turn black), then **Roll**; you get two re-rolls. Rows you can take show
what they would score, underlined. Tap a row to pick it, then tap it again or tap **Take** to
score it. Between turns the card follows whoever is playing; tap the `<` and `>` at the top, or
use the scroll keys, to page through the others. **Scores** lists everyone's total.

Keys: the scroll keys pick a row, the Date Book button rolls, and the other app buttons take the
picked row (or toggle Ready). Graffiti `1`-`5` hold a die and `r` rolls.

## Building

The toolchain is [prc-tools-remix](https://github.com/jichu4n/prc-tools-remix), the Palm OS SDKs
and PilRC in a Docker image (x86-64; Apple Silicon runs it under emulation):

```sh
make image    # once: builds the palmdev image from docker/Dockerfile
make          # build/Fujitzee.prc
make test     # the wire reader against replies saved from the live server
```

To install, copy the `.prc` to `/palm/install/` on FujiNet's SD card (the web UI's WebDAV share
works: `curl -T build/Fujitzee.prc http://<fujinet>/dav/palm/install/`) and HotSync.

## Layout

| File | Contents |
|---|---|
| `Fujitzee.c` | the app: setup form, table screen, polling and moves |
| `Fujitzee.rcp`, `FujitzeeRsc.h` | forms, menus and alerts for PilRC |
| `fzwire.c`, `fzwire.h` | the server's binary replies (field by field, since a 68000 cannot read the packed int16s at odd offsets), its URLs and the scorecard labels; portable C89 |
| `common/` | FujiBus framing, FujiNet over the cradle, and `N:` devices, copied unchanged from [fujinet-palm](https://github.com/dillera/fujinet-palm) `common/` |
| `tools/make_icon.py`, `docker/Dockerfile` | the launcher icon and the toolchain image, also from fujinet-palm |
| `tests/` | `fujitzee_test.c` and the replies it reads, saved from `https://fujitzee.carr-designs.com/` |

## The wire

The state is `Game` from `src/misc.h`, 95 bytes plus 42 per player. The rules this client follows
are the ones `channelf/README.md` sets out: a move rides its own request and the reply is the next
state; `keepRoll` is the echo of the last roll, so the hold mask is staged locally and restaged
only when a roll comes back or a turn starts; a turn can arrive with the rolls already spent; the
upper total, bonus and total are never selectable; and the running total is summed here, since the
server fills slot 15 only at game over (`round` 99).
