package com.audiobookbridge.soundscape.engine

import com.audiobookbridge.soundscape.model.Scene
import com.audiobookbridge.soundscape.model.SceneRules
import kotlin.math.abs
import kotlin.math.min
import kotlin.math.sin

/**
 * Pure (Android-free) model of the two ambience players and their crossfade.
 *
 * Two slots, A and B. A scene change fades the new scene in on one slot while
 * the other fades out, over [fadeMs]. The caller drives time with [advance]
 * and copies [outputs] onto the real players; nothing here touches audio, so
 * the whole thing is unit-tested on the JVM.
 *
 * Rules:
 *  - Requesting the scene that is already fading in only updates intensity.
 *  - Requesting the scene that is currently fading *out* reverses that fade
 *    from where it is (no restart, no jump).
 *  - Requesting a third scene while both slots are busy reuses the quieter
 *    slot; the louder one keeps fading out from its current level.
 *  - [Scene.NONE] fades everything out.
 *  - While [paused], time stands still: fades resume exactly where they were.
 *
 * Final volume per slot =
 *   equalPower(level) * intensityGain * master * narrationVolume * sleepFade * duck
 */
class CrossfadeMachine(
    private val fadeMs: Long = SceneRules.CROSSFADE_MS,
    private val duckRampMs: Long = 300L,
) {
    class Slot internal constructor(val index: Int) {
        /** Scene loaded in this slot; NONE means the slot is idle and its player can stop. */
        var scene: Scene = Scene.NONE
            internal set
        /** Fade position 0..1 (before the equal-power curve). */
        var level: Float = 0f
            internal set
        var target: Float = 0f
            internal set
        var intensity: Float = 1f
            internal set
        var intensityTarget: Float = 1f
            internal set
        /** Increments every time a new scene is loaded into this slot. */
        var loadToken: Int = 0
            internal set

        override fun toString() =
            "Slot$index($scene level=$level->$target int=$intensity->$intensityTarget tok=$loadToken)"
    }

    /** What the real player for one slot should be doing right now. */
    data class SlotOutput(
        val index: Int,
        val scene: Scene,
        val loadToken: Int,
        val volume: Float,
        val shouldPlay: Boolean,
    )

    val slots: List<Slot> = listOf(Slot(0), Slot(1))

    /** Last requested scene (what the user hears once fades settle). */
    var targetScene: Scene = Scene.NONE
        private set
    var targetIntensity: Float = 1f
        private set

    var paused: Boolean = false
        private set

    /** User's ambience slider, 0..1 relative to the narration. */
    var master: Float = SceneRules.DEFAULT_MASTER
        set(value) { field = value.coerceIn(0f, 1f) }

    /** Mirrors the narration player's own volume (sleep timers usually ramp this). */
    var narrationVolume: Float = 1f
        set(value) { field = value.coerceIn(0f, 1f) }

    /** Extra multiplier for sleep timers that fade by other means. 1 = no fade. */
    var sleepFade: Float = 1f
        set(value) { field = value.coerceIn(0f, 1f) }

    /** True while the narration is buffering. Ramped over [duckRampMs]. */
    var ducked: Boolean = false

    /** Current duck amount 0..1 (1 = fully ducked by [SceneRules.BUFFERING_DUCK]). */
    var duckLevel: Float = 0f
        private set

    /** Slot that is fading in or fully in, if any. */
    val activeSlot: Slot? get() = slots.firstOrNull { it.target > 0f }

    /** True while anything is still moving, so the caller knows to keep ticking. */
    val isAnimating: Boolean
        get() = !paused && (
            slots.any { it.level != it.target || it.intensity != it.intensityTarget } ||
                duckLevel != (if (ducked) 1f else 0f)
            )

    /** Progress of the incoming fade, 0..1 (1 when settled). */
    val fadeProgress: Float get() = activeSlot?.level ?: (1f - (slots.maxOfOrNull { it.level } ?: 0f))

    /**
     * Asks for [scene] at [intensity]. Returns true if anything changed.
     */
    fun request(scene: Scene, intensity: Float = 1f): Boolean {
        val i = intensity.coerceIn(0f, 1f)
        val changedTarget = scene != targetScene || i != targetIntensity
        targetScene = scene
        targetIntensity = i

        if (scene == Scene.NONE) {
            var changed = false
            for (s in slots) if (s.target != 0f) { s.target = 0f; changed = true }
            return changed || changedTarget
        }

        val active = activeSlot
        if (active != null && active.scene == scene) {
            active.intensityTarget = i
            return changedTarget
        }

        // Same scene already present but fading out: reverse that fade in place.
        val existing = slots.firstOrNull { it.scene == scene && it.level > 0f }
        if (existing != null) {
            existing.target = 1f
            existing.intensityTarget = i
            for (s in slots) if (s !== existing) s.target = 0f
            return true
        }

        // Load into an idle slot if there is one, otherwise the quieter one.
        val slot = slots.firstOrNull { it.scene == Scene.NONE || it.level == 0f && it.target == 0f }
            ?: slots.minBy { it.level }
        slot.scene = scene
        slot.level = 0f
        slot.target = 1f
        slot.intensity = i
        slot.intensityTarget = i
        slot.loadToken++
        for (s in slots) if (s !== slot) s.target = 0f
        return true
    }

    fun pause() { paused = true }
    fun resume() { paused = false }

    /** Moves fades forward by [dtMs] of real time. Does nothing while paused. */
    fun advance(dtMs: Long) {
        if (paused || dtMs <= 0) return
        val step = dtMs.toFloat() / fadeMs.coerceAtLeast(1)
        for (s in slots) {
            s.level = approach(s.level, s.target, step)
            s.intensity = approach(s.intensity, s.intensityTarget, step)
            if (s.level == 0f && s.target == 0f) s.scene = Scene.NONE
        }
        val duckStep = dtMs.toFloat() / duckRampMs.coerceAtLeast(1)
        duckLevel = approach(duckLevel, if (ducked) 1f else 0f, duckStep)
    }

    /** Snaps every fade to its end state (used when the engine is restarted). */
    fun settle() {
        for (s in slots) {
            s.level = s.target
            s.intensity = s.intensityTarget
            if (s.level == 0f) s.scene = Scene.NONE
        }
        duckLevel = if (ducked) 1f else 0f
    }

    fun gain(slot: Slot): Float {
        if (slot.scene == Scene.NONE) return 0f
        val curve = sin(slot.level * (Math.PI.toFloat() / 2f))
        val duck = 1f - SceneRules.BUFFERING_DUCK * duckLevel
        return (curve * SceneRules.intensityGain(slot.intensity) * master * narrationVolume * sleepFade * duck)
            .coerceIn(0f, 1f)
    }

    fun outputs(): List<SlotOutput> = slots.map { s ->
        SlotOutput(
            index = s.index,
            scene = s.scene,
            loadToken = s.loadToken,
            volume = gain(s),
            shouldPlay = !paused && s.scene != Scene.NONE && (s.level > 0f || s.target > 0f),
        )
    }

    private fun approach(value: Float, target: Float, step: Float): Float {
        val d = target - value
        // The epsilon stops float rounding from leaving a sliver of fade that never finishes.
        return if (abs(d) <= step + SNAP_EPS) target else value + min(step, abs(d)) * (if (d > 0) 1f else -1f)
    }

    private companion object {
        const val SNAP_EPS = 1e-4f
    }
}
