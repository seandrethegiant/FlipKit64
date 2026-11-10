package com.audiobookbridge.soundscape.engine

import com.audiobookbridge.soundscape.model.Scene
import com.audiobookbridge.soundscape.model.SoundscapeTimeline
import com.audiobookbridge.soundscape.model.TimelineSegment
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SceneDirectorTest {

    private fun seg(startS: Int, endS: Int, scene: Scene, i: Float = 0.5f) =
        TimelineSegment(startS * 1000L, endS * 1000L, scene, i)

    private val timeline = SoundscapeTimeline(
        bookId = "book",
        version = 1,
        segments = listOf(
            seg(0, 120, Scene.RAIN),
            seg(120, 150, Scene.TAVERN),   // 30 s: kept by the model, but too soon for hysteresis
            seg(150, 400, Scene.FOREST),
            seg(400, 410, Scene.BATTLE),   // 10 s: dropped by normalize (< 20 s)
            seg(410, 900, Scene.CAVE, 0.8f),
        ),
    )

    /** Plays from [fromS] to [toS] polling every 500 ms of wall time at [speed]. Returns the cues. */
    private fun play(d: SceneDirector, fromS: Int, toS: Int, speed: Double = 1.0): List<Pair<Long, SceneDirector.Cue>> {
        val cues = mutableListOf<Pair<Long, SceneDirector.Cue>>()
        var pos = fromS * 1000.0
        while (pos < toS * 1000.0) {
            d.update(pos.toLong())?.let { cues += pos.toLong() to it }
            pos += 500 * speed
        }
        return cues
    }

    @Test
    fun `first update applies the scene immediately`() {
        val d = SceneDirector().apply { this.timeline = this@SceneDirectorTest.timeline }
        assertEquals(Scene.RAIN, d.update(5_000)?.scene)
        assertNull(d.update(5_500))
    }

    @Test
    fun `short segments are ignored and switches are at least 45 s apart`() {
        val d = SceneDirector().apply { this.timeline = this@SceneDirectorTest.timeline }
        val cues = play(d, 0, 900)
        val scenes = cues.map { it.second.scene }
        assertEquals(listOf(Scene.RAIN, Scene.TAVERN, Scene.FOREST, Scene.CAVE), scenes)
        // TAVERN starts at 120 s; FOREST is wanted at 150 s but is held until 165 s.
        assertEquals(120_000L, cues[1].first)
        assertEquals(165_000L, cues[2].first)
        // BATTLE (10 s) never appears; CAVE starts where BATTLE was absorbed into FOREST.
        assertEquals(410_000L, cues[3].first)
        for (k in 1 until cues.size) {
            assertEquals(true, cues[k].first - cues[k - 1].first >= 45_000)
        }
    }

    @Test
    fun `speed does not change which scene plays at a book position`() {
        for (speed in listOf(0.5, 1.0, 2.0, 3.0)) {
            val d = SceneDirector().apply { this.timeline = this@SceneDirectorTest.timeline }
            val cues = play(d, 0, 900, speed)
            assertEquals("speed $speed", listOf(Scene.RAIN, Scene.TAVERN, Scene.FOREST, Scene.CAVE), cues.map { it.second.scene })
            // Switch points stay within one poll of book time.
            assertEquals(410_000.0, cues.last().first.toDouble(), 500 * speed + 1)
        }
    }

    @Test
    fun `seek lands on the right scene immediately, ignoring hysteresis`() {
        val d = SceneDirector().apply { this.timeline = this@SceneDirectorTest.timeline }
        d.update(10_000)
        // Forward jump detected without a flag.
        assertEquals(Scene.CAVE, d.update(500_000)?.scene)
        // Backward jump.
        assertEquals(Scene.RAIN, d.update(30_000)?.scene)
    }

    @Test
    fun `small move flagged as a seek bypasses hysteresis`() {
        val d = SceneDirector().apply { this.timeline = this@SceneDirectorTest.timeline }
        d.update(100_000)
        assertNull(d.update(118_000 - 3_000))
        assertNull(d.update(118_000))
        // 3 s forward, not detected as a jump: TAVERN is held (only 21 s since the last switch)...
        assertNull(d.update(121_000))
        // ...but the same position reported as a seek (e.g. a chapter tap) switches now.
        assertEquals(Scene.TAVERN, d.update(121_000, seeked = true)?.scene)
    }

    @Test
    fun `seek right after a switch still switches`() {
        val d = SceneDirector().apply { this.timeline = this@SceneDirectorTest.timeline }
        val cues = play(d, 70, 121)
        assertEquals(listOf(Scene.RAIN, Scene.TAVERN), cues.map { it.second.scene })
        // A seek 10 s after that switch is applied at once.
        assertEquals(Scene.FOREST, d.update(200_000, seeked = true)?.scene)
    }

    @Test
    fun `past the end and in gaps the scene is NONE`() {
        val d = SceneDirector().apply { this.timeline = this@SceneDirectorTest.timeline }
        assertEquals(Scene.NONE, d.update(5_000_000)?.scene)
    }

    @Test
    fun `intensity changes inside a scene pass through without hysteresis`() {
        val t = SoundscapeTimeline(
            "b", 1,
            listOf(seg(0, 60, Scene.STORM, 0.2f), seg(60, 120, Scene.STORM, 0.9f)),
        )
        // Same scene, so normalize merges them (weighted intensity).
        assertEquals(1, t.effectiveSegments.size)
        val d = SceneDirector().apply { timeline = t }
        val cue = d.update(1_000)!!
        assertEquals(Scene.STORM, cue.scene)
        assertEquals(0.55f, cue.intensity, 1e-3f)
    }
}
