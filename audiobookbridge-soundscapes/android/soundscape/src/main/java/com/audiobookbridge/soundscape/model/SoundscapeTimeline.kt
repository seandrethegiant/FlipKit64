package com.audiobookbridge.soundscape.model

/** One stretch of book time with a scene. Times are book-global milliseconds. */
data class TimelineSegment(
    val startMs: Long,
    val endMs: Long,
    val scene: Scene,
    val intensity: Float,
) {
    val durationMs: Long get() = endMs - startMs
    operator fun contains(positionMs: Long): Boolean = positionMs in startMs until endMs
}

/**
 * Scene timeline for one book, as produced by soundscape_gen.py and served at
 * `/soundscapes/<jellyfinItemId>.json`.
 *
 * `version` is the file's schema version. The app reads [SUPPORTED_VERSION]
 * and rejects anything newer, so an old app never misreads a new format.
 */
data class SoundscapeTimeline(
    val bookId: String,
    val version: Int,
    val segments: List<TimelineSegment>,
) {
    /** Sorted copy of the segments with the app's rules applied (see [normalize]). */
    private val clean: List<TimelineSegment> = normalize(segments)

    /** The cleaned segments the engine actually uses. */
    val effectiveSegments: List<TimelineSegment> get() = clean

    /** Segment covering [positionMs], or null when the position is in a gap / past the end. */
    fun segmentAt(positionMs: Long): TimelineSegment? {
        var lo = 0
        var hi = clean.size - 1
        while (lo <= hi) {
            val mid = (lo + hi) ushr 1
            val s = clean[mid]
            when {
                positionMs < s.startMs -> hi = mid - 1
                positionMs >= s.endMs -> lo = mid + 1
                else -> return s
            }
        }
        return null
    }

    companion object {
        const val SUPPORTED_VERSION = 1

        /** Gaps up to this size between two segments count as touching. */
        private const val TOUCH_TOLERANCE_MS = 1_000L

        /**
         * Sorts, drops invalid segments, merges neighbours with the same scene and
         * absorbs segments shorter than [SceneRules.MIN_SEGMENT_MS] into the previous
         * one (or the next one, at the very start). Overlaps are trimmed so the
         * result is strictly increasing and non-overlapping.
         */
        fun normalize(input: List<TimelineSegment>): List<TimelineSegment> {
            val sorted = input
                .filter { it.endMs > it.startMs && it.startMs >= 0 }
                .map { it.copy(intensity = it.intensity.coerceIn(0f, 1f)) }
                .sortedBy { it.startMs }
            if (sorted.isEmpty()) return emptyList()

            // Trim overlaps.
            val trimmed = ArrayList<TimelineSegment>(sorted.size)
            for (s in sorted) {
                val prev = trimmed.lastOrNull()
                val start = if (prev != null && s.startMs < prev.endMs) prev.endMs else s.startMs
                if (s.endMs > start) trimmed += s.copy(startMs = start)
            }

            var out = mergeSame(trimmed)

            // Absorb short segments until none are left (each pass shrinks the list).
            while (true) {
                val idx = out.indexOfFirst { it.durationMs < SceneRules.MIN_SEGMENT_MS }
                if (idx < 0 || out.size == 1) break
                val short = out[idx]
                val list = out.toMutableList()
                val prev = list.getOrNull(idx - 1)
                val next = list.getOrNull(idx + 1)
                when {
                    prev != null && short.startMs - prev.endMs <= TOUCH_TOLERANCE_MS ->
                        list[idx - 1] = prev.copy(endMs = short.endMs)
                    next != null && next.startMs - short.endMs <= TOUCH_TOLERANCE_MS ->
                        list[idx + 1] = next.copy(startMs = short.startMs)
                    // Isolated short segment surrounded by gaps: just drop it.
                }
                list.removeAt(idx)
                out = mergeSame(list)
            }
            return out
        }

        private fun mergeSame(list: List<TimelineSegment>): List<TimelineSegment> {
            val out = ArrayList<TimelineSegment>(list.size)
            for (s in list) {
                val prev = out.lastOrNull()
                if (prev != null && prev.scene == s.scene && s.startMs - prev.endMs <= TOUCH_TOLERANCE_MS) {
                    val total = (prev.durationMs + s.durationMs).coerceAtLeast(1)
                    val intensity = (prev.intensity * prev.durationMs + s.intensity * s.durationMs) / total
                    out[out.size - 1] = prev.copy(endMs = s.endMs, intensity = intensity)
                } else {
                    out += s
                }
            }
            return out
        }
    }
}
