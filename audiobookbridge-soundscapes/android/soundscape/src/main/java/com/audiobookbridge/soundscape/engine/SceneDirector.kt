package com.audiobookbridge.soundscape.engine

import com.audiobookbridge.soundscape.model.Scene
import com.audiobookbridge.soundscape.model.SceneRules
import com.audiobookbridge.soundscape.model.SoundscapeTimeline
import kotlin.math.abs

/**
 * Pure (Android-free) "which scene should be playing at this book position".
 *
 * Feed it the narration position in **book time** (not wall-clock time) every
 * ~500 ms. Because it only ever looks at book position, playback speed does not
 * matter: at 3x the position just moves faster.
 *
 * - Seeks / chapter jumps land on the right scene immediately. A seek is either
 *   flagged by the caller or detected as a jump (backwards, or forwards by more
 *   than [seekThresholdMs] between two updates).
 * - Otherwise scenes switch at most once per [minSwitchMs] of book time. A
 *   change that arrives too early is held, then applied once the interval has
 *   passed (if the timeline still wants it).
 * - Intensity changes within the same scene are passed through (the
 *   crossfade machine ramps them).
 */
class SceneDirector(
    private val minSwitchMs: Long = SceneRules.MIN_SWITCH_INTERVAL_MS,
    private val seekThresholdMs: Long = 5_000L,
    private val backwardToleranceMs: Long = 1_500L,
) {
    data class Cue(val scene: Scene, val intensity: Float)

    var timeline: SoundscapeTimeline? = null
        set(value) { field = value; reset() }

    var currentScene: Scene? = null
        private set
    private var currentIntensity = 0f
    private var lastSwitchAtMs = 0L
    private var lastPositionMs = -1L

    /** Forget history so the next [update] applies its scene immediately. */
    fun reset() {
        currentScene = null
        lastPositionMs = -1L
    }

    /** Returns a cue when the engine should change scene or intensity, else null. */
    fun update(positionMs: Long, seeked: Boolean = false): Cue? {
        val pos = positionMs.coerceAtLeast(0)
        val jumped = seeked || lastPositionMs < 0 ||
            pos < lastPositionMs - backwardToleranceMs ||
            pos > lastPositionMs + seekThresholdMs
        lastPositionMs = pos

        val seg = timeline?.segmentAt(pos)
        val desired = seg?.scene ?: Scene.NONE
        val desiredIntensity = seg?.intensity ?: 0f

        if (currentScene == null || jumped) {
            val changed = desired != currentScene || abs(desiredIntensity - currentIntensity) > INTENSITY_EPS
            currentScene = desired
            currentIntensity = desiredIntensity
            lastSwitchAtMs = pos
            return if (changed) Cue(desired, desiredIntensity) else null
        }

        if (desired == currentScene) {
            if (abs(desiredIntensity - currentIntensity) > INTENSITY_EPS) {
                currentIntensity = desiredIntensity
                return Cue(desired, desiredIntensity)
            }
            return null
        }

        if (pos - lastSwitchAtMs >= minSwitchMs) {
            currentScene = desired
            currentIntensity = desiredIntensity
            lastSwitchAtMs = pos
            return Cue(desired, desiredIntensity)
        }
        return null // held by hysteresis
    }

    private companion object {
        const val INTENSITY_EPS = 0.01f
    }
}
