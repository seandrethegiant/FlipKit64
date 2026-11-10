package com.audiobookbridge.soundscape.engine

import com.audiobookbridge.soundscape.model.Scene
import com.audiobookbridge.soundscape.model.SceneRules

/** How the engine picks its scene. */
enum class SoundscapeMode {
    /** The user picked a scene by hand (or the timeline is unavailable). */
    MANUAL,
    /** Scenes follow the book's timeline from the PC. */
    AUTO,
}

/**
 * Public, read-only snapshot of the engine, exposed as a StateFlow.
 *
 * This is also the Phase 4 "Storylit hook": a bulb controller can collect
 * [SoundscapeEngine.state] and map [scene] / [intensity] to colour and
 * brightness, ramping over [crossfadeMs] so the lights fade with the sound.
 */
data class SoundscapeState(
    val enabled: Boolean = false,
    val mode: SoundscapeMode = SoundscapeMode.MANUAL,
    /** Scene the engine is heading to (the one you hear once a fade settles). */
    val scene: Scene = Scene.NONE,
    /** Intensity of [scene], 0..1. */
    val intensity: Float = 1f,
    /** True while ambience is actually audible (narration playing, scene not NONE). */
    val playing: Boolean = false,
    /** True while the narration is buffering and the ambience is ducked. */
    val ducked: Boolean = false,
    /** User's ambience level 0..1 relative to the narration. */
    val master: Float = SceneRules.DEFAULT_MASTER,
    /** A timeline is loaded for the current book. */
    val timelineAvailable: Boolean = false,
    val crossfadeMs: Long = SceneRules.CROSSFADE_MS,
)
