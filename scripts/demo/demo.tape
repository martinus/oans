# oans demo — VHS screenplay (https://github.com/charmbracelet/vhs).
# Rendered by record.sh, which runs `vhs` from a work dir containing ./oans and
# ./tree. VHS is the highest-quality path (real typing, window chrome, exact
# timing). To render by hand:  cd <workdir> && vhs /path/to/demo.tape
#
# VHS emits the GIF; record.sh then optimizes it with gifsicle (a quality-neutral
# ~30% shrink — the smallest inline asset for terminal casts).
Output demo.gif

Set Shell bash
Set FontFamily "Hack"
Set FontSize 20
Set Width 1280
Set Height 760
Set Padding 24
Set Theme "Dracula"
Set WindowBar Colorful
Set BorderRadius 10
Set TypingSpeed 55ms
Set Framerate 12   # terminal casts read fine at 12fps and it roughly halves the GIF

# Put oans on PATH (exposed by record.sh) without filming the plumbing.
Hide
Type "export PATH=${OANS_BIN_DIR:-$PWD}:$PATH; clear"
Enter
Show

Sleep 1s

# 1) First run: hash + dedupe the tree. Live progress + honest summary.
#    Wait for the summary's "Elapsed" line — printed on every dedupe run whatever
#    the headline wording — rather than a fixed sleep, so this stays correct
#    however long a bigger tree takes to scan.
Type "oans -dr --hashfile=demo.hash tree"
Sleep 500ms
Enter
Wait+Screen@600s /Elapsed/
Sleep 5s

# 2) What the hashfile holds and how much is reclaimable.
Type "oans --stats --hashfile=demo.hash"
Sleep 500ms
Enter
Sleep 6s

# 3) The headline: a re-run skips everything already shared — near-instant.
Type "oans --hashfile=demo.hash"
Sleep 500ms
Enter
Sleep 6s

# 4) Space reclaimed over time.
Type "oans --history --hashfile=demo.hash"
Sleep 500ms
Enter
Sleep 6s

Sleep 1500ms
