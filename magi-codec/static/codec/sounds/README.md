# Call sounds (optional)

Put your own audio files here to replace the synthesized sounds. The call page
uses whichever of these exist (first match by extension: mp3, ogg, wav, m4a,
aac, webm, opus) and synthesizes the rest:

| File          | When it plays                                   | Tips                                  |
|---------------|-------------------------------------------------|---------------------------------------|
| `ring.*`      | Incoming ring, looped until you tap ANSWER      | Trim so it loops cleanly              |
| `answer.*`    | Ring-to-connect after tapping ANSWER            | Text starts when it ends (max 4 s)    |
| `blip.*`      | Each typed character (every other one)          | Keep it under 50 ms                   |
| `close.*`     | Static-out on hang up                           | Return to dashboard after it (max 3 s) |

Everything in this folder except this README is git-ignored, so your files stay
on your PC and are never pushed. No restart needed: reload the call page.
