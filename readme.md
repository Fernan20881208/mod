# Click Indicators

Shows you where a macro clicks instead of clicking for you. It never touches your input, so it is a practice and memory tool, not a bot.

Presses appear in the level as vertical bars, and fall down a rhythm lane at the edge of the screen. Every click you make gets scored against the macro.

## Importing a macro

Pause in any level, open the macro list, and hit **Import Macro**.

On **Android** that opens the system file picker: choose one or more macros and they are copied in for you. This is the only way in on Android — the macros folder lives inside the game's private storage, where a file manager cannot browse to it, so being shown the path was advice you could not act on.

On **Windows** it opens the macros folder in Explorer instead, so you can drop a whole batch in at once and then hit **Refresh**. The folder is `Geometry Dash\geode\config\bogdoner.click-indicators\macros`, created the first time you launch the game with the mod installed.

Files picked on Android are checked by parsing them, not by their extension, because the Android picker cannot filter by type. Anything that is not a macro this mod can read is skipped and named in the summary.

Formats read: `.gdr2` (GDR v2), `.gdr` and `.xd` (xdBot / GDR v1), and `.slc` (Silicate v2 and v3).

### How a macro gets matched to a level

In order, best first:

1. the level ID stored inside the macro file
2. the level name stored inside the macro file
3. the level ID appearing in the filename, so `13519.slc` and `13519 - clean run.slc` both work
4. the filename matching the level name, ignoring case and punctuation

Silicate never writes the level ID or name into its files, and plenty of `.gdr` recordings leave them blank too, so for those the **filename is what matches**. Name it after the level ID.

You can always override all of this: pause, open the macro list, and hit Load on the file you want. An explicit pick beats any automatic match and is remembered for that level.

### Nothing showing up

Check the log at `Geometry Dash\geode\logs`. On opening a level the mod prints every file it read, what level each one claims, and if nothing matched, the full list of what is in the folder. That tells you straight away whether it is a folder problem, a parse problem, or a naming problem.


### Macros that lie about their framerate

Some bots write their own recording setting into the macro's framerate field rather than the rate the frame numbers are actually counted in. MEGA does this: a macro saved with the game set to 1200 says "1200 tps" while its frame numbers are plain physics steps at 240. Read literally, the whole macro collapses to a fifth of its true length and every indicator lands early and bunched together.

The mod cross-checks the declared rate against how long the level takes to walk. When the two disagree by a clean factor it says so in the log and uses the rate that fits. It only does this when the declared rate is *above* 240, which is the only direction a bot can overstate, so a genuine recording of part of a level is never rescaled by mistake.

None of this has anything to do with your monitor. Everything is laid out in seconds, so the refresh rate you play at does not change the spacing of anything.

## Timing

The indicator runs on `m_timePlayed`, the game's own play timer — the number behind the in-game time display. A macro recorded from the start of a level is a list of moments measured in gameplay seconds, and that timer is gameplay seconds. They are the same quantity, so there is nothing to model, nothing to integrate, and nothing to drift.

Position is consulted exactly once per attempt, and only to answer "how far into the macro does this attempt begin". It never touches the clock again. Position feedback is what dragged the old clock off: a timeline built from a wrong start speed runs fast, and it took the indicators with it.

Nothing about the clock is derived from the level model any more, so a level whose speed portals the mod reads wrongly still keeps perfect time on a run from the start.

The level bars are a separate matter: they place a time back onto a position, so they do depend on the speed timeline. That timeline now starts from the opening speed written in the level's own header rather than a guess. Before, a level opening at 0.5x or 4x was assumed to be 1x until the mod had watched you play it once, and bars laid out against a too-fast assumption creep towards you as you go. That only ever showed up on a machine that had not played the level before, which is why it looked like a "works here, not there" problem.

### Start positions and checkpoints

Starting mid-level is the one case that needs an answer to "how far into the macro is this spot", and position is the only thing that can answer it.

The mod measures how fast you actually move through each slice of the level and integrates that to get the time. Every attempt contributes, including one that starts at a start position deep into the level, because speed at a position is a property of the level rather than of the run. Slices nobody has reached yet fall back to the speed portal model.

That model is not good enough on its own. On a big level it can be one or two percent out, which is a second or more by the time you are two thirds through — hundreds of frames, when the scoring windows are tens of milliseconds. So when the mod resumes it logs how much of its answer was measured and how much was estimated. If a start position sits in a stretch you have never played through, the estimate is the error you will feel.

The fix for that is **Align**, below. Levels with reverse gameplay are a known gap: there a position is two different moments and there is no honest answer.

### Timing offset and Align

You should not need either for a normal run. A macro recorded from the start of a level lines up on its own and the default offset of zero is correct.

**Align** is for start positions in territory the mod has had to estimate. Play the section through once, clicking as you normally would, then pause and hit Align. It takes the rhythm of your clicks and slides it along the macro until it locks on.

The rhythm is what makes it work. Matching a single click to the nearest press only helps when the macro is already close, because "nearest" stops meaning anything once the error is larger than the gap between presses. Several clicks are unambiguous where one is not — only the true offset makes all of them land at once. Align falls back to single-click matching if you have only clicked once, and refuses rather than guessing if the best fit it can find is still loose.

The `-1f` / `+1f` buttons nudge from there, **Reset** puts the offset back to zero, and **Timing offset** in the settings shifts every indicator by a fixed number of milliseconds if you want to lead or trail deliberately.

## Reading the indicators

Aim at the **left edge** of a bar. That is the press. The right edge is the release, so a long hold is a wide bar and you hold it the whole way across.

In the rhythm lane the notes fall downward, so the **bottom** of a note is the press. When it touches the hit line, that is the frame.

The line through your icon is your reference point. A bar touching it is the exact click frame.

## Scoring

Every press is matched to the nearest macro press that has not been answered yet. Your click is timed in the update it actually arrived in, rather than worked backwards from when the draw loop got round to it, so the number you are shown is the real error and not an estimate of it.

- **PERFECT** — within the perfect window
- **OK** — outside that but within the OK window, shown with how many milliseconds early or late you were
- **MISS** — too far off, or a macro press you never answered

The tally under the verdict is perfect / OK / miss for the attempt.

Windows are set in milliseconds rather than frames, so they mean the same thing on a 60 tps recording and a 1200 tps one.

## Dual levels

Macros with two input streams draw player 2 in its own colour, and the rhythm lane splits into two columns. Presses are matched per player, so hitting the wrong side counts as a miss rather than quietly matching the other player's press.

Player 2 notes are shown only while the level is actually dual. They appear when you go through a dual portal and stop when you come out of one, so a level with a single dual section no longer carries a second column of notes for the rest of the run, and a macro with stray P2 inputs on a single player level shows none at all.

The trade is that P2 notes appear at the dual portal rather than scrolling in ahead of it. The mod knows the level is dual from the game, which only knows once you are in it.

## Platformer levels

Platformer macros load, and their jump presses are shown on the rhythm lane. The level bars are turned off there: they place notes by converting a time back into an x position, and in a platformer you can stand still, walk backwards and be teleported, so there is no position to convert to. The lane is the honest view.

Left and right movement inputs are not drawn. There is nowhere to put them.

## Settings worth touching first

- **Indicator transparency** — 0 is solid, 100 is invisible. Default 80. Push it higher if the bars crowd the level art.
- **Lane window** — how much time the rhythm lane covers. Lower it for faster, more precise notes.
- **Look ahead** — how far into the future the level bars go.

## Drawing

The level bars are drawn above the level, not inside it, and they refuse the colour and opacity cascade. A fade trigger, a dark section or a colour trigger cannot dim or tint them, and deco cannot be painted over them. They sit below the game's own UI, so nothing covers the pause button or the progress bar.

Nothing is drawn until the level has been scanned and the clock anchored, which is a frame or two at level load. A blank frame is better than a frame in the wrong place followed by a jump — especially on a level whose first click comes early enough for that jump to be confusing.

The speed timeline is built once at load and never rebuilt while you play. When the mod learns something about the level mid-run it caches it for the next load rather than respacing bars you are in the middle of reading.
