# clay-bricks

The Clay package manager, written in Clay — a port of `src/compiler/bricks.c`.
`clayc bricks <command>` does what `clay bricks <command>` does: the same
`brick.json` and `brick.lock`, the same checks on names, URLs and flags, the
same messages (naming `clayc` where clay names itself). CTest
(`clay_selfhost_bricks`) runs both on the same commands against git
repositories it creates locally, and compares what they print, their exit
codes, and the files they write.

```clay norun
import("../clay-bricks/src/main.clay")

System exit(Bricks command(["add", "github.com/owner/repo"]))
```

`Bricks command(args)` runs `init`, `add`, `install`, `update`, `list`,
`remove`, `search` or `info` and returns the exit code. Set `Bricks prog` to
change the program name used in messages.

See the comment at the top of `src/main.clay` for the few differences from
`bricks.c` (git runs without a shell, the registry JSON is read by a small
parser) and the quirks kept (the lenient `brick.json` reader; rewriting
`brick.json` keeps only the fields Clay knows).
