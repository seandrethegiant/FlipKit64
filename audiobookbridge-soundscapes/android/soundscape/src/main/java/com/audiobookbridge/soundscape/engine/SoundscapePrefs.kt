package com.audiobookbridge.soundscape.engine

import android.content.Context
import com.audiobookbridge.soundscape.model.Scene
import com.audiobookbridge.soundscape.model.SceneRules

/**
 * Soundscape settings. Volume, on/off and per-book choices are kept per
 * Jellyfin user ([userId]); the PC URL and "auto scenes" are device-wide.
 */
class SoundscapePrefs(
    context: Context,
    private val userId: () -> String?,
) {
    private val sp = context.applicationContext.getSharedPreferences("soundscape", Context.MODE_PRIVATE)

    private fun u(key: String) = "u.${userId() ?: "default"}.$key"
    private fun book(bookId: String, key: String) = u("book.$bookId.$key")

    var master: Float
        get() = sp.getFloat(u("master"), SceneRules.DEFAULT_MASTER)
        set(v) = sp.edit().putFloat(u("master"), v.coerceIn(0f, 1f)).apply()

    var enabled: Boolean
        get() = sp.getBoolean(u("enabled"), true)
        set(v) = sp.edit().putBoolean(u("enabled"), v).apply()

    var baseUrl: String
        get() = sp.getString("baseUrl", "") ?: ""
        set(v) = sp.edit().putString("baseUrl", v.trim()).apply()

    var autoScenes: Boolean
        get() = sp.getBoolean("autoScenes", true)
        set(v) = sp.edit().putBoolean("autoScenes", v).apply()

    /** Scene picked by hand for this book (NONE if never picked). */
    fun manualScene(bookId: String): Scene =
        Scene.parse(sp.getString(book(bookId, "scene"), null)) ?: Scene.NONE

    fun setManualScene(bookId: String, scene: Scene) =
        sp.edit().putString(book(bookId, "scene"), scene.name).apply()

    /** Whether this book follows its timeline. Defaults to true; picking a scene by hand turns it off. */
    fun useTimeline(bookId: String): Boolean = sp.getBoolean(book(bookId, "auto"), true)

    fun setUseTimeline(bookId: String, on: Boolean) =
        sp.edit().putBoolean(book(bookId, "auto"), on).apply()
}
