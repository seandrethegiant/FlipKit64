package com.audiobookbridge.soundscape.model

/**
 * Reads and writes [SoundscapeTimeline] JSON without any library, so the
 * module adds no dependency and the parser runs in plain JVM unit tests
 * (Android's org.json is only a stub there).
 *
 * Format:
 * ```
 * { "bookId": "...", "version": 1,
 *   "segments": [ { "startMs": 0, "endMs": 61000, "scene": "RAIN", "intensity": 0.6 } ] }
 * ```
 * Unknown keys are ignored. Segments with an unknown scene are skipped, not fatal.
 */
object TimelineJson {

    class FormatException(message: String) : Exception(message)

    fun parse(text: String): SoundscapeTimeline {
        val root = MiniJson(text).parseRoot() as? Map<*, *>
            ?: throw FormatException("root is not an object")
        val version = (root["version"] as? Number)?.toInt() ?: 1
        if (version > SoundscapeTimeline.SUPPORTED_VERSION) {
            throw FormatException("unsupported timeline version $version")
        }
        val bookId = root["bookId"] as? String ?: throw FormatException("missing bookId")
        val rawSegments = root["segments"] as? List<*> ?: throw FormatException("missing segments")
        val segments = rawSegments.mapNotNull { raw ->
            val m = raw as? Map<*, *> ?: return@mapNotNull null
            val start = (m["startMs"] as? Number)?.toLong() ?: return@mapNotNull null
            val end = (m["endMs"] as? Number)?.toLong() ?: return@mapNotNull null
            val scene = Scene.parse(m["scene"] as? String) ?: return@mapNotNull null
            val intensity = (m["intensity"] as? Number)?.toFloat() ?: 1f
            TimelineSegment(start, end, scene, intensity)
        }
        return SoundscapeTimeline(bookId, version, segments)
    }

    fun write(timeline: SoundscapeTimeline): String = buildString {
        append("{\"bookId\":").append(quote(timeline.bookId))
        append(",\"version\":").append(timeline.version)
        append(",\"segments\":[")
        timeline.segments.forEachIndexed { i, s ->
            if (i > 0) append(',')
            append("{\"startMs\":").append(s.startMs)
            append(",\"endMs\":").append(s.endMs)
            append(",\"scene\":").append(quote(s.scene.name))
            append(",\"intensity\":").append(s.intensity)
            append('}')
        }
        append("]}")
    }

    private fun quote(s: String): String = buildString {
        append('"')
        for (c in s) when {
            c == '"' -> append("\\\"")
            c == '\\' -> append("\\\\")
            c < ' ' -> append(String.format("\\u%04x", c.code))
            else -> append(c)
        }
        append('"')
    }

    /** Small strict JSON reader: objects, arrays, strings, numbers, booleans, null. */
    private class MiniJson(private val s: String) {
        private var i = 0

        fun parseRoot(): Any? {
            val v = value()
            ws()
            if (i != s.length) fail("trailing data")
            return v
        }

        private fun fail(msg: String): Nothing = throw FormatException("$msg at $i")

        private fun ws() {
            while (i < s.length && s[i].isWhitespace()) i++
        }

        private fun value(): Any? {
            ws()
            if (i >= s.length) fail("unexpected end")
            return when (val c = s[i]) {
                '{' -> obj()
                '[' -> arr()
                '"' -> str()
                't' -> literal("true", true)
                'f' -> literal("false", false)
                'n' -> literal("null", null)
                else -> if (c == '-' || c.isDigit()) num() else fail("unexpected '$c'")
            }
        }

        private fun literal(word: String, v: Any?): Any? {
            if (!s.startsWith(word, i)) fail("bad literal")
            i += word.length
            return v
        }

        private fun obj(): Map<String, Any?> {
            val m = LinkedHashMap<String, Any?>()
            i++ // {
            ws()
            if (i < s.length && s[i] == '}') { i++; return m }
            while (true) {
                ws()
                if (i >= s.length || s[i] != '"') fail("expected key")
                val k = str()
                ws()
                if (i >= s.length || s[i] != ':') fail("expected ':'")
                i++
                m[k] = value()
                ws()
                if (i >= s.length) fail("unterminated object")
                when (s[i++]) {
                    ',' -> continue
                    '}' -> return m
                    else -> fail("expected ',' or '}'")
                }
            }
        }

        private fun arr(): List<Any?> {
            val l = ArrayList<Any?>()
            i++ // [
            ws()
            if (i < s.length && s[i] == ']') { i++; return l }
            while (true) {
                l += value()
                ws()
                if (i >= s.length) fail("unterminated array")
                when (s[i++]) {
                    ',' -> continue
                    ']' -> return l
                    else -> fail("expected ',' or ']'")
                }
            }
        }

        private fun str(): String {
            val sb = StringBuilder()
            i++ // opening quote
            while (true) {
                if (i >= s.length) fail("unterminated string")
                val c = s[i++]
                when (c) {
                    '"' -> return sb.toString()
                    '\\' -> {
                        if (i >= s.length) fail("bad escape")
                        when (val e = s[i++]) {
                            '"' -> sb.append('"')
                            '\\' -> sb.append('\\')
                            '/' -> sb.append('/')
                            'b' -> sb.append('\b')
                            'f' -> sb.append('\u000C')
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            'u' -> {
                                if (i + 4 > s.length) fail("bad unicode escape")
                                sb.append(s.substring(i, i + 4).toInt(16).toChar())
                                i += 4
                            }
                            else -> fail("bad escape '$e'")
                        }
                    }
                    else -> sb.append(c)
                }
            }
        }

        private fun num(): Number {
            val start = i
            if (s[i] == '-') i++
            while (i < s.length && (s[i].isDigit() || s[i] in ".eE+-")) i++
            val text = s.substring(start, i)
            return text.toLongOrNull() ?: text.toDoubleOrNull() ?: fail("bad number '$text'")
        }
    }
}
