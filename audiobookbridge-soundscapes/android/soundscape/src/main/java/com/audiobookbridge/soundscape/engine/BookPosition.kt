package com.audiobookbridge.soundscape.engine

import androidx.media3.common.C
import androidx.media3.common.Player
import androidx.media3.common.Timeline

/**
 * Turns the narration player's position into **book-global** time.
 *
 * A Jellyfin audiobook is usually an album whose tracks are chapters, queued
 * as several media items. The player's `currentPosition` is per item, so the
 * offsets of the earlier items have to be added. Single-file books (one m4b)
 * just return `currentPosition`.
 */
typealias BookPositionProvider = (Player) -> Long

object BookPosition {

    /**
     * Sums the durations of the media items before the current one, as the
     * player knows them. Items whose duration is not known yet count as 0, so
     * prefer [fromChapterDurations] when you have Jellyfin's RunTimeTicks.
     */
    val fromPlayerTimeline: BookPositionProvider = { player ->
        val timeline = player.currentTimeline
        val index = player.currentMediaItemIndex
        var offset = 0L
        if (!timeline.isEmpty) {
            val window = Timeline.Window()
            for (i in 0 until index.coerceAtMost(timeline.windowCount)) {
                val d = timeline.getWindow(i, window).durationMs
                if (d != C.TIME_UNSET) offset += d
            }
        }
        offset + player.currentPosition.coerceAtLeast(0)
    }

    /**
     * Uses known chapter (track) durations in queue order, e.g. from Jellyfin
     * `RunTimeTicks / 10_000`. These must be the same durations the PC
     * generator used, so times line up exactly.
     */
    fun fromChapterDurations(durationsMs: List<Long>): BookPositionProvider {
        val offsets = LongArray(durationsMs.size + 1)
        for (i in durationsMs.indices) offsets[i + 1] = offsets[i] + durationsMs[i].coerceAtLeast(0)
        return { player ->
            val index = player.currentMediaItemIndex.coerceIn(0, durationsMs.size)
            offsets[index] + player.currentPosition.coerceAtLeast(0)
        }
    }
}
