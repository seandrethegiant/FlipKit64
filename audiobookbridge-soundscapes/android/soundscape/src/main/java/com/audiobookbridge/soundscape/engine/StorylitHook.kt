package com.audiobookbridge.soundscape.engine

import com.audiobookbridge.soundscape.model.Scene
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map

/**
 * Phase 4 hook for Storylit (smart-bulb control).
 *
 * ```
 * scope.launch {
 *     engine.lightCues().collect { cue ->
 *         storylit.fadeTo(cue.rgb, cue.brightness, durationMs = cue.fadeMs)
 *     }
 * }
 * ```
 * A cue is emitted only when the scene, intensity or playing state changes.
 * Ramp the bulbs over [LightCue.fadeMs] so they move with the sound.
 */
data class LightCue(
    val scene: Scene,
    val intensity: Float,
    /** False while the narration is paused/stopped or ambience is off: dim or restore the room. */
    val active: Boolean,
    val rgb: Int,
    /** 0..1, already scaled by intensity. */
    val brightness: Float,
    val fadeMs: Long,
)

fun SoundscapeEngine.lightCues(palette: (Scene) -> Int = ::defaultSceneColor): Flow<LightCue> =
    state
        .map { s ->
            val active = s.enabled && s.playing && s.scene != Scene.NONE
            LightCue(
                scene = s.scene,
                intensity = s.intensity,
                active = active,
                rgb = palette(s.scene),
                brightness = if (active) 0.25f + 0.6f * s.intensity.coerceIn(0f, 1f) else 0f,
                fadeMs = s.crossfadeMs,
            )
        }
        .distinctUntilChanged()

/** A starting palette (0xRRGGBB). Storylit can supply its own. */
fun defaultSceneColor(scene: Scene): Int = when (scene) {
    Scene.NONE -> 0xFFE2C0          // warm white
    Scene.RAIN -> 0x5A7FA8
    Scene.STORM -> 0x2E3A5C
    Scene.FOREST -> 0x3F7A3A
    Scene.NIGHT -> 0x1C2450
    Scene.OCEAN -> 0x1F7A99
    Scene.CITY -> 0xC9A86A
    Scene.TAVERN -> 0xE08A2E
    Scene.FIRE -> 0xFF6A1A
    Scene.CAVE -> 0x4A4038
    Scene.WIND -> 0x9DB4C0
    Scene.SPACESHIP -> 0x3AC8D8
    Scene.BATTLE -> 0xB0261E
    Scene.ARCHIVE_QUIET -> 0xD9B77A
    Scene.RIVER -> 0x4FA3A0
    Scene.WINTER -> 0xCFE3F5
    Scene.TRAVEL -> 0xB98A4E
    Scene.VEHICLE -> 0x6B7A8F
    Scene.HOME -> 0xF2C38B
}
