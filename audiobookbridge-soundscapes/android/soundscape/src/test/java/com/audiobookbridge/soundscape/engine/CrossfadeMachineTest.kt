package com.audiobookbridge.soundscape.engine

import com.audiobookbridge.soundscape.model.Scene
import com.audiobookbridge.soundscape.model.SceneRules
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class CrossfadeMachineTest {

    private lateinit var m: CrossfadeMachine
    private val fade = SceneRules.CROSSFADE_MS

    @Before
    fun setUp() {
        m = CrossfadeMachine(fadeMs = fade)
        m.master = 1f // makes volumes easy to read
    }

    private fun out(scene: Scene) = m.outputs().single { it.scene == scene }
    private fun scenes() = m.outputs().map { it.scene }.filter { it != Scene.NONE }.toSet()

    /** Advances in 50 ms steps like the real ticker. */
    private fun run(ms: Long) {
        var left = ms
        while (left > 0) {
            val dt = minOf(50L, left)
            m.advance(dt)
            left -= dt
        }
    }

    @Test
    fun `first scene fades in from silence over the crossfade time`() {
        m.request(Scene.RAIN)
        assertEquals(0f, out(Scene.RAIN).volume, 1e-6f)
        assertTrue(out(Scene.RAIN).shouldPlay)

        run(fade / 2)
        val mid = out(Scene.RAIN).volume
        assertTrue("half way should be partly up, was $mid", mid > 0.5f && mid < 0.9f)

        run(fade / 2)
        assertEquals(1f, out(Scene.RAIN).volume, 1e-4f)
        assertFalse(m.isAnimating)
    }

    @Test
    fun `scene A to B crossfades with equal power and frees A at the end`() {
        m.request(Scene.RAIN); run(fade)
        m.request(Scene.TAVERN)
        assertEquals(setOf(Scene.RAIN, Scene.TAVERN), scenes())
        // Different slots.
        assertTrue(out(Scene.RAIN).index != out(Scene.TAVERN).index)

        run(fade / 2)
        val a = out(Scene.RAIN).volume
        val b = out(Scene.TAVERN).volume
        assertEquals("equal power: a^2 + b^2 = 1", 1f, a * a + b * b, 1e-3f)
        assertEquals(a, b, 1e-3f)

        run(fade / 2)
        assertEquals(setOf(Scene.TAVERN), scenes())
        assertEquals(1f, out(Scene.TAVERN).volume, 1e-4f)
        assertEquals(Scene.TAVERN, m.targetScene)
    }

    @Test
    fun `switching to NONE fades out and leaves both slots idle`() {
        m.request(Scene.FOREST); run(fade)
        m.request(Scene.NONE)
        assertTrue(m.isAnimating)
        run(fade / 2)
        assertTrue(out(Scene.FOREST).volume in 0.01f..0.99f)
        run(fade / 2)
        assertTrue(scenes().isEmpty())
        assertTrue(m.outputs().none { it.shouldPlay })
        assertFalse(m.isAnimating)
    }

    @Test
    fun `rapid switching A to B to C keeps the louder slot fading out and never uses a third player`() {
        m.request(Scene.RAIN); run(fade)
        m.request(Scene.TAVERN); run(fade / 4) // RAIN loud, TAVERN quiet
        val rainBefore = out(Scene.RAIN).volume
        val tavernSlot = out(Scene.TAVERN).index

        m.request(Scene.CAVE)
        // Quieter slot (TAVERN) was reused for CAVE; RAIN keeps fading from where it was.
        assertEquals(setOf(Scene.RAIN, Scene.CAVE), scenes())
        assertEquals(tavernSlot, out(Scene.CAVE).index)
        assertEquals(rainBefore, out(Scene.RAIN).volume, 1e-6f)
        assertEquals(0f, out(Scene.CAVE).volume, 1e-6f)

        run(fade)
        assertEquals(setOf(Scene.CAVE), scenes())
        assertEquals(1f, out(Scene.CAVE).volume, 1e-4f)
    }

    @Test
    fun `many switches in one tick end on the last request`() {
        m.request(Scene.RAIN)
        m.request(Scene.STORM)
        m.request(Scene.OCEAN)
        m.request(Scene.CITY)
        m.request(Scene.FIRE)
        assertTrue(m.outputs().size == 2)
        run(fade)
        assertEquals(setOf(Scene.FIRE), scenes())
        assertEquals(Scene.FIRE, m.targetScene)
    }

    @Test
    fun `switching back mid-fade reverses the fade in place`() {
        m.request(Scene.RAIN); run(fade)
        val rainSlot = out(Scene.RAIN).index
        val rainToken = out(Scene.RAIN).loadToken
        m.request(Scene.TAVERN); run(fade / 4)
        val rainMid = out(Scene.RAIN).volume

        m.request(Scene.RAIN)
        // Same slot, same load (no restart), same volume at the moment of the switch.
        assertEquals(rainSlot, out(Scene.RAIN).index)
        assertEquals(rainToken, out(Scene.RAIN).loadToken)
        assertEquals(rainMid, out(Scene.RAIN).volume, 1e-6f)

        run(fade)
        assertEquals(setOf(Scene.RAIN), scenes())
        assertEquals(1f, out(Scene.RAIN).volume, 1e-4f)
    }

    @Test
    fun `pause mid-fade freezes volumes and resume continues from the same point`() {
        m.request(Scene.RAIN); run(fade)
        m.request(Scene.NIGHT); run(fade / 2)
        val rain = out(Scene.RAIN).volume
        val night = out(Scene.NIGHT).volume

        m.pause()
        assertTrue(m.outputs().none { it.shouldPlay })
        assertFalse(m.isAnimating)
        run(10_000) // time passes while paused
        assertEquals(rain, out(Scene.RAIN).volume, 1e-6f)
        assertEquals(night, out(Scene.NIGHT).volume, 1e-6f)

        m.resume()
        assertTrue(out(Scene.NIGHT).shouldPlay)
        assertTrue(m.isAnimating)
        run(fade / 2)
        assertEquals(setOf(Scene.NIGHT), scenes())
        assertEquals(1f, out(Scene.NIGHT).volume, 1e-4f)
    }

    @Test
    fun `scene request while paused waits silently until resume`() {
        m.pause()
        m.request(Scene.OCEAN)
        run(fade)
        assertEquals(0f, out(Scene.OCEAN).volume, 1e-6f)
        assertFalse(out(Scene.OCEAN).shouldPlay)
        m.resume()
        run(fade)
        assertEquals(1f, out(Scene.OCEAN).volume, 1e-4f)
    }

    @Test
    fun `re-requesting the active scene only changes intensity, smoothly`() {
        m.request(Scene.STORM, 1f); run(fade)
        val token = out(Scene.STORM).loadToken
        m.request(Scene.STORM, 0f)
        assertEquals(token, out(Scene.STORM).loadToken)
        run(fade / 2)
        val mid = out(Scene.STORM).volume
        assertTrue(mid < 1f && mid > SceneRules.INTENSITY_FLOOR)
        run(fade / 2)
        assertEquals(SceneRules.INTENSITY_FLOOR, out(Scene.STORM).volume, 1e-4f)
    }

    @Test
    fun `master, narration volume and sleep fade multiply`() {
        m.request(Scene.WIND); run(fade)
        m.master = 0.15f
        assertEquals(0.15f, out(Scene.WIND).volume, 1e-4f)
        m.narrationVolume = 0.5f // sleep timer ramping the narration player
        assertEquals(0.075f, out(Scene.WIND).volume, 1e-4f)
        m.sleepFade = 0f
        assertEquals(0f, out(Scene.WIND).volume, 1e-6f)
    }

    @Test
    fun `buffering ducks by an extra 30 percent, ramped`() {
        m.request(Scene.CITY); run(fade)
        m.master = 0.15f
        m.ducked = true
        assertTrue(m.isAnimating)
        run(1_000)
        assertEquals(0.15f * 0.7f, out(Scene.CITY).volume, 1e-4f)
        m.ducked = false
        run(1_000)
        assertEquals(0.15f, out(Scene.CITY).volume, 1e-4f)
    }

    @Test
    fun `slot load token changes only when a new file must be loaded`() {
        m.request(Scene.RAIN)
        val t1 = out(Scene.RAIN).loadToken
        run(fade)
        m.request(Scene.RAIN)
        assertEquals(t1, out(Scene.RAIN).loadToken)

        m.request(Scene.CAVE); run(fade) // RAIN's slot is idle again
        val before = m.slots.map { it.loadToken }
        m.request(Scene.RAIN)
        val slot = out(Scene.RAIN).index
        assertEquals(before[slot] + 1, out(Scene.RAIN).loadToken)
    }
}
