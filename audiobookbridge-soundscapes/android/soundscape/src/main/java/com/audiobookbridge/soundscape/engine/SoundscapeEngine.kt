package com.audiobookbridge.soundscape.engine

import android.content.Context
import android.os.SystemClock
import android.util.Log
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import com.audiobookbridge.soundscape.model.Scene
import com.audiobookbridge.soundscape.model.SceneRules
import com.audiobookbridge.soundscape.model.SoundscapeTimeline
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlin.random.Random

/**
 * Ambient sound bed under the narration.
 *
 * Owns two looping ExoPlayers (slot A / B, for crossfades) that are completely
 * separate from the narration player: it never touches the narration's queue,
 * volume or audio focus. It only *listens* to the narration player and follows
 * it: pause, resume, stop, buffering (duck) and volume changes (sleep-timer
 * fade-out).
 *
 * Audio focus: the narration player keeps handling focus. The ambience players
 * are built with `handleAudioFocus = false`, so they never request focus and
 * never fight the narration for it. When the narration loses focus it pauses
 * (or is suppressed) and the ambience pauses with it.
 *
 * Threading: create and call this on the narration player's application
 * thread (normally the main thread), with a [scope] that dispatches there,
 * e.g. the MediaSessionService's `lifecycleScope` or
 * `CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)`.
 *
 * Failure policy: every entry point is wrapped; a soundscape problem is logged
 * and the ambience goes quiet, the narration is never affected.
 */
class SoundscapeEngine(
    context: Context,
    private val narration: Player,
    private val scope: CoroutineScope,
    bookPosition: BookPositionProvider = BookPosition.fromPlayerTimeline,
) {
    private val appContext = context.applicationContext
    private val machine = CrossfadeMachine()
    private val director = SceneDirector()
    private val players = arrayOfNulls<ExoPlayer>(2)
    private val appliedTokens = intArrayOf(NO_TOKEN, NO_TOKEN)
    /** Load token that last errored per slot, so a broken file isn't retried every tick. */
    private val failedTokens = intArrayOf(NO_TOKEN, NO_TOKEN)
    private val needsRandomStart = booleanArrayOf(false, false)

    private var tickJob: Job? = null
    private var pollJob: Job? = null
    private var released = false

    private var enabled = false
    private var mode = SoundscapeMode.MANUAL
    private var manualScene = Scene.NONE
    private var timeline: SoundscapeTimeline? = null
    private var pendingSeek = false
    private var narrationActive = false

    /** How narration position maps to book time; swap it when the book changes. */
    var bookPosition: BookPositionProvider = bookPosition

    private val _state = MutableStateFlow(SoundscapeState())

    /** Current scene / intensity / mode. Also the Phase 4 Storylit hook. */
    val state: StateFlow<SoundscapeState> = _state.asStateFlow()

    private val narrationListener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) {
            if (events.containsAny(
                    Player.EVENT_PLAYBACK_STATE_CHANGED,
                    Player.EVENT_PLAY_WHEN_READY_CHANGED,
                    Player.EVENT_IS_PLAYING_CHANGED,
                    Player.EVENT_PLAYBACK_SUPPRESSION_REASON_CHANGED,
                    Player.EVENT_VOLUME_CHANGED,
                )
            ) guard("narration events") { syncWithNarration() }
        }

        override fun onPositionDiscontinuity(
            oldPosition: Player.PositionInfo,
            newPosition: Player.PositionInfo,
            reason: Int,
        ) {
            if (reason == Player.DISCONTINUITY_REASON_SEEK ||
                reason == Player.DISCONTINUITY_REASON_SEEK_ADJUSTMENT ||
                reason == Player.DISCONTINUITY_REASON_SKIP ||
                reason == Player.DISCONTINUITY_REASON_REMOVE
            ) guard("seek") {
                pendingSeek = true
                updateFromTimeline() // land on the right scene now, even while paused
            }
        }
    }

    init {
        guard("init") {
            narration.addListener(narrationListener)
            syncWithNarration()
        }
    }

    // ---------------------------------------------------------------- public API

    /** Master on/off. Off fades the ambience out. */
    fun setEnabled(on: Boolean) = guard("setEnabled") {
        if (enabled == on) return@guard
        enabled = on
        applyTarget()
    }

    /** Ambience level 0..1 relative to the narration (default 0.15). */
    fun setMasterVolume(level: Float) = guard("setMaster") {
        machine.master = level
        applyOutputs()
        publish()
    }

    /** Manual mode with this scene (NONE fades out). */
    fun selectScene(scene: Scene) = guard("selectScene") {
        manualScene = scene
        mode = SoundscapeMode.MANUAL
        applyTarget()
    }

    /**
     * Auto mode: follow [timeline]. If [timeline] is null the engine stays in
     * (or falls back to) manual mode with the last manual scene.
     */
    fun followTimeline(timeline: SoundscapeTimeline?) = guard("followTimeline") {
        this.timeline = timeline
        director.timeline = timeline
        mode = if (timeline != null) SoundscapeMode.AUTO else SoundscapeMode.MANUAL
        applyTarget()
    }

    /** Sets the scene used whenever the engine is (or falls back to) manual mode. */
    fun setManualScene(scene: Scene) = guard("setManualScene") {
        manualScene = scene
        if (mode == SoundscapeMode.MANUAL) applyTarget()
    }

    /**
     * Optional: for sleep timers that do not ramp the narration player's
     * volume. 1 = full, 0 = silent. Timers that ramp `player.volume` are
     * followed automatically and need no call.
     */
    fun setSleepFade(fraction: Float) = guard("setSleepFade") {
        machine.sleepFade = fraction
        applyOutputs()
    }

    /** Stops everything and frees both ambience players. */
    fun release() {
        if (released) return
        released = true
        runCatching { narration.removeListener(narrationListener) }
        tickJob?.cancel()
        pollJob?.cancel()
        for (i in players.indices) {
            runCatching { players[i]?.release() }
            players[i] = null
        }
    }

    // ---------------------------------------------------------------- internals

    private fun syncWithNarration() {
        val state = narration.playbackState
        narrationActive = narration.playWhenReady &&
            (state == Player.STATE_READY || state == Player.STATE_BUFFERING) &&
            narration.playbackSuppressionReason == Player.PLAYBACK_SUPPRESSION_REASON_NONE
        machine.ducked = state == Player.STATE_BUFFERING
        machine.narrationVolume = narration.volume
        if (narrationActive) machine.resume() else machine.pause()
        updatePolling()
        applyOutputs()
        ensureTicking()
        publish()
    }

    /** Pushes the scene the current mode wants into the crossfade machine. */
    private fun applyTarget() {
        if (!enabled) {
            machine.request(Scene.NONE)
        } else if (mode == SoundscapeMode.AUTO && timeline != null) {
            director.reset()
            updateFromTimeline()
        } else {
            machine.request(manualScene, 1f)
        }
        updatePolling()
        applyOutputs()
        ensureTicking()
        publish()
    }

    private fun updateFromTimeline() {
        if (!enabled || mode != SoundscapeMode.AUTO || timeline == null) return
        val pos = try {
            bookPosition(narration)
        } catch (t: Throwable) {
            Log.w(TAG, "book position failed", t)
            return
        }
        val cue = director.update(pos, pendingSeek)
        pendingSeek = false
        if (cue != null) {
            machine.request(cue.scene, cue.intensity)
            applyOutputs()
            ensureTicking()
            publish()
        }
    }

    private fun updatePolling() {
        val want = !released && enabled && mode == SoundscapeMode.AUTO && timeline != null && narrationActive
        if (!want) {
            pollJob?.cancel()
            pollJob = null
            return
        }
        if (pollJob?.isActive == true) return
        pollJob = scope.launch {
            while (isActive) {
                guard("poll") { updateFromTimeline() }
                delay(POLL_MS)
            }
        }
    }

    private fun ensureTicking() {
        if (released || !machine.isAnimating || tickJob?.isActive == true) return
        tickJob = scope.launch {
            var last = SystemClock.elapsedRealtime()
            while (isActive && machine.isAnimating) {
                delay(TICK_MS)
                val now = SystemClock.elapsedRealtime()
                guard("tick") {
                    machine.advance(now - last)
                    applyOutputs()
                }
                last = now
            }
            guard("tick end") { applyOutputs(); publish() }
        }
    }

    /** Copies the machine's slot outputs onto the two real players. */
    private fun applyOutputs() {
        if (released) return
        for (out in machine.outputs()) {
            try {
                applySlot(out)
            } catch (t: Throwable) {
                Log.e(TAG, "ambience slot ${out.index} failed; silencing it", t)
                failedTokens[out.index] = out.loadToken
                silenceSlot(out.index)
            }
        }
    }

    private fun applySlot(out: CrossfadeMachine.SlotOutput) {
        val i = out.index
        if (out.scene == Scene.NONE) {
            if (appliedTokens[i] != NO_TOKEN) silenceSlot(i)
            return
        }
        if (out.loadToken == failedTokens[i]) return
        val player = players[i] ?: createPlayer(i).also { players[i] = it }
        if (appliedTokens[i] != out.loadToken) {
            val asset = out.scene.assetPath ?: return
            player.setMediaItem(MediaItem.fromUri("asset:///$asset"))
            player.volume = 0f
            player.prepare()
            needsRandomStart[i] = true
            appliedTokens[i] = out.loadToken
        }
        player.volume = out.volume
        if (player.playWhenReady != out.shouldPlay) player.playWhenReady = out.shouldPlay
    }

    private fun silenceSlot(i: Int) {
        appliedTokens[i] = NO_TOKEN
        players[i]?.let { p ->
            runCatching {
                p.volume = 0f
                p.stop()
                p.clearMediaItems()
            }
        }
    }

    private fun createPlayer(slot: Int): ExoPlayer {
        val attrs = AudioAttributes.Builder()
            .setUsage(C.USAGE_MEDIA)
            .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
            .build()
        return ExoPlayer.Builder(appContext)
            .setLooper(narration.applicationLooper)
            .setAudioAttributes(attrs, /* handleAudioFocus = */ false)
            .setHandleAudioBecomingNoisy(false) // the narration pauses; we follow it
            .build()
            .apply {
                repeatMode = Player.REPEAT_MODE_ONE
                addListener(object : Player.Listener {
                    override fun onPlaybackStateChanged(state: Int) {
                        // Start each loop at a random point so the same seconds don't repeat on every scene change.
                        if (state == Player.STATE_READY && needsRandomStart[slot]) {
                            needsRandomStart[slot] = false
                            val d = duration
                            if (d != C.TIME_UNSET && d > 10_000) {
                                runCatching { seekTo(Random.nextLong(0, d - 5_000)) }
                            }
                        }
                    }

                    override fun onPlayerError(error: PlaybackException) {
                        Log.e(TAG, "ambience player $slot error: ${error.errorCodeName}", error)
                        failedTokens[slot] = appliedTokens[slot]
                        silenceSlot(slot)
                    }
                })
            }
    }

    private fun publish() {
        _state.value = SoundscapeState(
            enabled = enabled,
            mode = if (mode == SoundscapeMode.AUTO && timeline != null) SoundscapeMode.AUTO else SoundscapeMode.MANUAL,
            scene = machine.targetScene,
            intensity = machine.targetIntensity,
            playing = narrationActive && machine.targetScene != Scene.NONE,
            ducked = machine.ducked,
            master = machine.master,
            timelineAvailable = timeline != null,
            crossfadeMs = SceneRules.CROSSFADE_MS,
        )
    }

    private inline fun guard(what: String, block: () -> Unit) {
        if (released) return
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
        const val NO_TOKEN = -1
        const val TICK_MS = 33L
        const val POLL_MS = 500L
    }
}
