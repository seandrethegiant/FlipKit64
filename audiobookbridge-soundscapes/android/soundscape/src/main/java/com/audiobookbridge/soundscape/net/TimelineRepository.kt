package com.audiobookbridge.soundscape.net

import android.content.Context
import android.util.Log
import com.audiobookbridge.soundscape.model.SoundscapeTimeline
import com.audiobookbridge.soundscape.model.TimelineJson
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * Loads `GET <baseUrl>/soundscapes/<jellyfinItemId>.json` from the PC and
 * caches it in `filesDir/soundscapes/`. Uses plain HttpURLConnection, so it
 * adds no dependency.
 *
 * Nothing here throws to the caller: every failure becomes a [FetchResult]
 * (or null for the cache), and all I/O runs on Dispatchers.IO with short
 * timeouts, so playback is never blocked.
 */
class TimelineRepository(
    context: Context,
    private val baseUrl: () -> String?,
) {
    private val dir = File(context.applicationContext.filesDir, "soundscapes")

    sealed interface FetchResult {
        data class Updated(val timeline: SoundscapeTimeline) : FetchResult
        /** Server says our cached copy is current (HTTP 304). */
        data object NotModified : FetchResult
        /** Server reachable, no timeline for this book (HTTP 404). Cache removed. */
        data object Missing : FetchResult
        /** No base URL set, PC offline, timeout, bad JSON... */
        data class Failed(val reason: String) : FetchResult
    }

    /** Cached timeline for [bookId], or null. Corrupt files are deleted. */
    suspend fun cached(bookId: String): SoundscapeTimeline? = withContext(Dispatchers.IO) {
        val f = jsonFile(bookId)
        if (!f.isFile) return@withContext null
        try {
            TimelineJson.parse(f.readText())
        } catch (e: Exception) {
            Log.w(TAG, "corrupt cached timeline for $bookId, deleting", e)
            f.delete()
            etagFile(bookId).delete()
            null
        }
    }

    /** Asks the PC for a newer timeline. Sends If-None-Match when we have a cached copy. */
    suspend fun fetch(bookId: String): FetchResult = withContext(Dispatchers.IO) {
        val base = normalizeBaseUrl(baseUrl()) ?: return@withContext FetchResult.Failed("no PC URL set")
        val url = "$base/soundscapes/${URLEncoder.encode(bookId, "UTF-8")}.json"
        var conn: HttpURLConnection? = null
        try {
            conn = open(url)
            val etag = etagFile(bookId).takeIf { it.isFile && jsonFile(bookId).isFile }?.readText()
            if (!etag.isNullOrBlank()) conn.setRequestProperty("If-None-Match", etag)
            when (val code = conn.responseCode) {
                HttpURLConnection.HTTP_OK -> {
                    val body = readLimited(conn)
                    val timeline = TimelineJson.parse(body) // validate before caching
                    dir.mkdirs()
                    atomicWrite(jsonFile(bookId), body)
                    conn.getHeaderField("ETag")?.let { atomicWrite(etagFile(bookId), it) }
                        ?: etagFile(bookId).delete()
                    FetchResult.Updated(timeline)
                }
                HttpURLConnection.HTTP_NOT_MODIFIED -> FetchResult.NotModified
                HttpURLConnection.HTTP_NOT_FOUND -> {
                    jsonFile(bookId).delete()
                    etagFile(bookId).delete()
                    FetchResult.Missing
                }
                else -> FetchResult.Failed("HTTP $code")
            }
        } catch (c: CancellationException) {
            throw c
        } catch (e: Exception) {
            Log.i(TAG, "timeline fetch for $bookId failed: ${e.message}")
            FetchResult.Failed(e.message ?: e.javaClass.simpleName)
        } finally {
            conn?.disconnect()
        }
    }

    /**
     * For the settings screen's "Test connection" button. Calls
     * `<baseUrl>/soundscapes/ping`, which the PC server answers with
     * `{"ok":true,"timelines":N}`.
     */
    suspend fun testConnection(url: String): Result<String> = withContext(Dispatchers.IO) {
        val base = normalizeBaseUrl(url) ?: return@withContext Result.failure(IOException("Enter the PC URL first"))
        var conn: HttpURLConnection? = null
        try {
            conn = open("$base/soundscapes/ping")
            val code = conn.responseCode
            if (code != HttpURLConnection.HTTP_OK) return@withContext Result.failure(IOException("PC answered HTTP $code"))
            val body = readLimited(conn)
            val count = Regex("\"timelines\"\\s*:\\s*(\\d+)").find(body)?.groupValues?.get(1)
            Result.success(if (count != null) "Connected: $count timelines on the PC" else "Connected")
        } catch (c: CancellationException) {
            throw c
        } catch (e: Exception) {
            Result.failure(IOException("Can't reach the PC: ${e.message ?: e.javaClass.simpleName}", e))
        } finally {
            conn?.disconnect()
        }
    }

    private fun open(url: String): HttpURLConnection =
        (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout = READ_TIMEOUT_MS
            instanceFollowRedirects = false
            useCaches = false
            setRequestProperty("Accept", "application/json")
        }

    private fun readLimited(conn: HttpURLConnection): String {
        val bytes = conn.inputStream.use { input ->
            val buf = java.io.ByteArrayOutputStream()
            val chunk = ByteArray(16 * 1024)
            while (true) {
                val n = input.read(chunk)
                if (n < 0) break
                buf.write(chunk, 0, n)
                if (buf.size() > MAX_BYTES) throw IOException("timeline too large")
            }
            buf.toByteArray()
        }
        return String(bytes, Charsets.UTF_8)
    }

    private fun atomicWrite(target: File, text: String) {
        val tmp = File(target.parentFile, target.name + ".tmp")
        tmp.writeText(text)
        if (!tmp.renameTo(target)) {
            target.delete()
            tmp.renameTo(target)
        }
    }

    private fun safeName(bookId: String) = bookId.replace(Regex("[^A-Za-z0-9_-]"), "_")
    private fun jsonFile(bookId: String) = File(dir, safeName(bookId) + ".json")
    private fun etagFile(bookId: String) = File(dir, safeName(bookId) + ".etag")

    companion object {
        private const val TAG = "Soundscape"
        private const val CONNECT_TIMEOUT_MS = 3_000
        private const val READ_TIMEOUT_MS = 5_000
        private const val MAX_BYTES = 4 * 1024 * 1024

        /** "mypc:8642/" -> "http://mypc:8642". Returns null for blank input. */
        fun normalizeBaseUrl(raw: String?): String? {
            val s = raw?.trim()?.trimEnd('/') ?: return null
            if (s.isEmpty()) return null
            return if (s.startsWith("http://") || s.startsWith("https://")) s else "http://$s"
        }
    }
}
