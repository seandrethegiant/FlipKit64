package com.audiobookbridge.soundscape.engine

import android.util.Log
import com.audiobookbridge.soundscape.model.Scene
import com.audiobookbridge.soundscape.model.SoundscapeTimeline
import com.audiobookbridge.soundscape.net.TimelineRepository
import com.audiobookbridge.soundscape.net.TimelineRepository.FetchResult
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** Everything the player chip and settings screen need, in one snapshot. */
data class SoundscapeUiState(
    val engine: SoundscapeState = SoundscapeState(),
    val bookId: String? = null,
    /** The scene the user picked for this book (shown when in manual mode). */
    val manualScene: Scene = Scene.NONE,
    /** This book is set to follow its timeline (when one exists and auto scenes are on). */
    val useTimeline: Boolean = true,
    /** A timeline exists for this book (cached or from the PC), whether or not it is followed. */
    val timelineLoaded: Boolean = false,
    val autoScenesSetting: Boolean = true,
    val baseUrl: String = "",
)

/**
 * Glue between the engine, the saved settings and the PC timeline.
 *
 * Create one next to the narration player (in the MediaSessionService) and
 * make it reachable from the UI the same way the app shares its player
 * (a DI singleton, the Application object, ...). All calls must be made on
 * the main thread, like the engine.
 *
 * Call [onBookChanged] whenever a different book starts playing.
 */
class SoundscapeController(
    val engine: SoundscapeEngine,
    private val prefs: SoundscapePrefs,
    private val repository: TimelineRepository,
    private val scope: CoroutineScope,
) {
    private val local = MutableStateFlow(
        SoundscapeUiState(autoScenesSetting = prefs.autoScenes, baseUrl = prefs.baseUrl),
    )
    private var loadJob: Job? = null
    private var timeline: SoundscapeTimeline? = null
    private var chapters: List<Long>? = null

    val uiState: StateFlow<SoundscapeUiState> =
        combine(engine.state, local) { e, l -> l.copy(engine = e) }
            .stateIn(scope, SharingStarted.Eagerly, local.value)

    init {
        engine.setMasterVolume(prefs.master)
        engine.setEnabled(prefs.enabled)
    }

    /**
     * A new book is playing. [chapterDurationsMs] are the queue's track
     * durations in order (Jellyfin RunTimeTicks / 10 000); pass null for a
     * single-file book or to use the player's own durations.
     */
    fun onBookChanged(bookId: String?, chapterDurationsMs: List<Long>? = null) = safe("onBookChanged") {
        loadJob?.cancel()
        timeline = null
        chapters = chapterDurationsMs
        engine.bookPosition = chapterDurationsMs?.let { BookPosition.fromChapterDurations(it) }
            ?: BookPosition.fromPlayerTimeline
        if (bookId == null) {
            local.value = local.value.copy(bookId = null, manualScene = Scene.NONE, timelineLoaded = false)
            engine.followTimeline(null)
            engine.selectScene(Scene.NONE)
            return@safe
        }
        val manual = prefs.manualScene(bookId)
        local.value = local.value.copy(
            bookId = bookId,
            manualScene = manual,
            useTimeline = prefs.useTimeline(bookId),
            timelineLoaded = false,
        )
        // Manual first: plays at once, and stays if no timeline turns up.
        engine.setManualScene(manual)
        engine.followTimeline(null)

        if (!prefs.autoScenes) return@safe
        loadJob = scope.launch {
            try {
                repository.cached(bookId)?.let { applyTimeline(bookId, it) }
                when (val r = repository.fetch(bookId)) {
                    is FetchResult.Updated -> applyTimeline(bookId, r.timeline)
                    FetchResult.Missing -> applyTimeline(bookId, null)
                    FetchResult.NotModified -> Unit
                    is FetchResult.Failed -> Log.i(TAG, "no fresh timeline (${r.reason}); using ${if (timeline != null) "cache" else "manual mode"}")
                }
            } catch (c: CancellationException) {
                throw c
            } catch (t: Throwable) {
                Log.e(TAG, "timeline load failed; staying in manual mode", t)
            }
        }
    }

    /** User picked a scene on the chip: manual mode for this book, remembered. */
    fun pickScene(scene: Scene) = safe("pickScene") {
        val id = local.value.bookId
        if (id != null) {
            prefs.setManualScene(id, scene)
            prefs.setUseTimeline(id, false)
        }
        if (scene != Scene.NONE && !prefs.enabled) setEnabled(true)
        local.value = local.value.copy(manualScene = scene, useTimeline = false)
        engine.selectScene(scene)
    }

    /** User picked "Auto" on the chip: follow this book's timeline again. */
    fun pickAuto() = safe("pickAuto") {
        val id = local.value.bookId ?: return@safe
        prefs.setUseTimeline(id, true)
        local.value = local.value.copy(useTimeline = true)
        if (!prefs.enabled) setEnabled(true)
        engine.followTimeline(if (prefs.autoScenes) timeline else null)
    }

    fun setEnabled(on: Boolean) = safe("setEnabled") {
        prefs.enabled = on
        engine.setEnabled(on)
    }

    fun setMaster(level: Float) = safe("setMaster") {
        prefs.master = level
        engine.setMasterVolume(level)
    }

    fun setBaseUrl(url: String) = safe("setBaseUrl") {
        prefs.baseUrl = url
        local.value = local.value.copy(baseUrl = prefs.baseUrl)
    }

    fun setAutoScenes(on: Boolean) = safe("setAutoScenes") {
        prefs.autoScenes = on
        local.value = local.value.copy(autoScenesSetting = on)
        val id = local.value.bookId
        if (!on) engine.followTimeline(null)
        else if (id != null) onBookChanged(id, chapters) // reload the timeline
    }

    suspend fun testConnection(url: String = prefs.baseUrl): Result<String> = repository.testConnection(url)

    private fun applyTimeline(bookId: String, t: SoundscapeTimeline?) {
        if (local.value.bookId != bookId) return
        timeline = t
        local.value = local.value.copy(timelineLoaded = t != null)
        val follow = t != null && prefs.autoScenes && prefs.useTimeline(bookId)
        engine.followTimeline(if (follow) t else null)
    }

    private inline fun safe(what: String, block: () -> Unit) {
        try {
            block()
        } catch (c: CancellationException) {
            throw c
        } catch (t: Throwable) {
            Log.e(TAG, "soundscape $what failed (narration unaffected)", t)
        }
    }

    private companion object {
        const val TAG = "Soundscape"
    }
}
