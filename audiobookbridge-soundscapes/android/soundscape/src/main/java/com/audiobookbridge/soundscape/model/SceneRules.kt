package com.audiobookbridge.soundscape.model

/**
 * Timing rules shared by the app and the PC generator. Keep these in sync with
 * MIN_SEGMENT_MS / MIN_SWITCH_MS in pc/soundscape_gen.py.
 */
object SceneRules {
    /** Segments shorter than this are ignored (absorbed into their neighbour). */
    const val MIN_SEGMENT_MS = 20_000L

    /** Never switch scenes more often than this (book time). Seeks reset it. */
    const val MIN_SWITCH_INTERVAL_MS = 45_000L

    /** Length of a scene-to-scene crossfade. */
    const val CROSSFADE_MS = 4_000L

    /** Default ambience level relative to the narration (15%). */
    const val DEFAULT_MASTER = 0.15f

    /** Extra reduction while the narration is buffering (30%). */
    const val BUFFERING_DUCK = 0.30f

    /** Lowest gain a scene plays at when its intensity is 0, so it never vanishes. */
    const val INTENSITY_FLOOR = 0.30f

    /** Maps intensity 0..1 to a gain multiplier in [INTENSITY_FLOOR, 1]. */
    fun intensityGain(intensity: Float): Float {
        val i = intensity.coerceIn(0f, 1f)
        return INTENSITY_FLOOR + (1f - INTENSITY_FLOOR) * i
    }
}
