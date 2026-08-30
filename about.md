# Click Indicators

Shows you where a macro clicks instead of clicking for you. It never touches your input, so it is a practice and memory tool, not a bot.

Presses appear in the level as vertical bars, and fall down a rhythm lane at the edge of the screen. Every click you make gets scored against the macro.

## Importing a macro

1. Open your Geode folder. On Windows that is inside your Geometry Dash install:
   `Geometry Dash\geode\config\bogdoner.click-indicators\macros`
   If the folder is not there yet, launch the game once and it will be created.
2. Drop your `.gdr2` file in. Do not rename it.
3. Open the level. That is it.

The mod reads the level ID stored inside the macro file and matches it to the level you opened, so the filename does not matter. You can keep as many macros in the folder as you like and the right one gets picked automatically.

If a macro was recorded without a level ID, name the file after the level ID instead, for example `13519. gdr2`, and it will match on that.

### Where do macros come from

Any bot that exports the GDR2 format: MegaHack, xdBot, and others. If your bot only exports `.gdr`, `.mhr`, `.re` or another format, convert it to `.gdr2` first. This mod reads `.gdr2` only.

### Nothing showing up

Check the log at `Geometry Dash\geode\logs`. On opening a level the mod prints which files it scanned and what level each one claims. That tells you straight away whether it is a folder problem or an ID mismatch.

Platformer macros are skipped. Horizontal lines mean nothing when the player can walk backwards.

## Reading the indicators

Aim at the **left edge** of a bar. That is the press. The right edge is the release, so a long hold is a wide bar and you hold it the whole way across.

In the rhythm lane the notes fall downward, so the **bottom** of a note is the press. When it touches the hit line, that is the frame.

The line through your icon is your reference point. A bar touching it is the exact click frame.

## Scoring

Every press is matched to the nearest macro press that has not been answered yet.

- **PERFECT** — within the perfect window, 2 frames by default
- **OK** — outside that but within the OK window, shown with how many frames early or late you were
- **MISS** — too far off, or a macro press you never answered

The tally under the verdict is perfect / OK / miss for the attempt.

If you are consistently off by the same amount in the same direction, that is not you, that is alignment. The frame count on the OK verdicts tells you how much. Put it into **Timing offset**, remembering that at 240 tps one frame is roughly 4 ms.

## Dual levels

Macros with two input streams draw player 2 in its own colour, and the rhythm lane splits into two columns. Presses are matched per player, so hitting the wrong side counts as a miss rather than quietly matching the other player's press.

## Speed portals

The mod reads every speed portal in the level at load and builds an exact time to position curve from them. Indicators stay put when you cross a portal instead of sliding around.

## Settings worth touching first

- **Indicator transparency** — 0 is solid, 100 is invisible. Default 80. Push it higher if the bars crowd the level art.
- **Lane window** — how much time the rhythm lane covers. Lower it for faster, more precise notes.
- **Look ahead** — how far into the future the level bars go.
