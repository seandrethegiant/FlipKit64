package com.audiobookbridge.soundscape.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class TimelineTest {

    @Test
    fun `parses the generator format and skips unknown scenes`() {
        val json = """
            {
              "bookId": "a1b2c3", "version": 1, "generator": "soundscape_gen 1.0",
              "segments": [
                {"startMs": 0, "endMs": 60000, "scene": "RAIN", "intensity": 0.4},
                {"startMs": 60000, "endMs": 180000, "scene": "archive_quiet", "intensity": 1},
                {"startMs": 180000, "endMs": 240000, "scene": "UNICORNS", "intensity": 0.5}
              ]
            }
        """.trimIndent()
        val t = TimelineJson.parse(json)
        assertEquals("a1b2c3", t.bookId)
        assertEquals(2, t.segments.size)
        assertEquals(Scene.ARCHIVE_QUIET, t.segments[1].scene)
        assertEquals(0.4f, t.segments[0].intensity, 1e-6f)
    }

    @Test
    fun `reads a file written by soundscape_gen py`() {
        val text = javaClass.classLoader!!.getResource("generator_sample.json")!!.readText()
        val t = TimelineJson.parse(text)
        assertEquals("book1", t.bookId)
        assertEquals(listOf(Scene.RAIN, Scene.TAVERN, Scene.CAVE), t.effectiveSegments.map { it.scene })
        assertEquals(Scene.TAVERN, t.segmentAt(240_000)?.scene)
        assertEquals(Scene.CAVE, t.segmentAt(1_049_999)?.scene)
    }

    @Test
    fun `round trips through write and parse`() {
        val t = SoundscapeTimeline(
            "x\"y", 1,
            listOf(TimelineSegment(0, 30_000, Scene.FIRE, 0.25f), TimelineSegment(30_000, 90_000, Scene.NIGHT, 1f)),
        )
        assertEquals(t, TimelineJson.parse(TimelineJson.write(t)))
    }

    @Test
    fun `rejects newer versions and garbage`() {
        assertThrows(TimelineJson.FormatException::class.java) {
            TimelineJson.parse("""{"bookId":"a","version":99,"segments":[]}""")
        }
        assertThrows(TimelineJson.FormatException::class.java) { TimelineJson.parse("{not json") }
        assertThrows(TimelineJson.FormatException::class.java) { TimelineJson.parse("""{"bookId":"a"}""") }
    }

    @Test
    fun `lookup finds the segment by book position`() {
        val t = SoundscapeTimeline(
            "b", 1,
            listOf(
                TimelineSegment(0, 60_000, Scene.RAIN, 1f),
                TimelineSegment(60_000, 200_000, Scene.CITY, 1f),
                TimelineSegment(300_000, 400_000, Scene.OCEAN, 1f), // gap before
            ),
        )
        assertEquals(Scene.RAIN, t.segmentAt(0)?.scene)
        assertEquals(Scene.RAIN, t.segmentAt(59_999)?.scene)
        assertEquals(Scene.CITY, t.segmentAt(60_000)?.scene)
        assertNull(t.segmentAt(250_000))
        assertEquals(Scene.OCEAN, t.segmentAt(399_999)?.scene)
        assertNull(t.segmentAt(400_000))
    }

    @Test
    fun `normalize absorbs short segments and merges same scenes`() {
        val n = SoundscapeTimeline.normalize(
            listOf(
                TimelineSegment(5_000, 15_000, Scene.STORM, 1f),   // short, first: joins next
                TimelineSegment(15_000, 75_000, Scene.RAIN, 1f),
                TimelineSegment(75_000, 85_000, Scene.CITY, 1f),   // short: joins RAIN
                TimelineSegment(85_000, 145_000, Scene.RAIN, 0f),  // then merges with RAIN
                TimelineSegment(145_000, 205_000, Scene.CAVE, 1f),
            ),
        )
        assertEquals(listOf(Scene.RAIN, Scene.CAVE), n.map { it.scene })
        assertEquals(5_000L, n[0].startMs)
        assertEquals(145_000L, n[0].endMs)
    }

    @Test
    fun `normalize trims overlaps and drops invalid segments`() {
        val n = SoundscapeTimeline.normalize(
            listOf(
                TimelineSegment(0, 100_000, Scene.FOREST, 2f),
                TimelineSegment(50_000, 200_000, Scene.WIND, -1f),
                TimelineSegment(300_000, 250_000, Scene.FIRE, 1f),
            ),
        )
        assertEquals(2, n.size)
        assertEquals(100_000L, n[1].startMs)
        assertEquals(1f, n[0].intensity)
        assertEquals(0f, n[1].intensity)
    }

    @Test
    fun `scene parse is forgiving`() {
        assertEquals(Scene.ARCHIVE_QUIET, Scene.parse(" archive-quiet "))
        assertEquals(Scene.SPACESHIP, Scene.parse("spaceship"))
        assertNull(Scene.parse("lava"))
        assertEquals("ambience/archive_quiet.ogg", Scene.ARCHIVE_QUIET.assetPath)
        assertNull(Scene.NONE.assetPath)
    }
}
