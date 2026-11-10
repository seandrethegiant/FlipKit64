package com.audiobookbridge.soundscape.model

/**
 * The fixed scene vocabulary. The PC generator (soundscape_gen.py) uses exactly
 * these names, so add new values in both places (and a matching asset).
 */
enum class Scene(val label: String) {
    NONE("Off"),
    RAIN("Rain"),
    STORM("Storm"),
    FOREST("Forest"),
    NIGHT("Night"),
    OCEAN("Ocean"),
    RIVER("River"),
    WINTER("Winter"),
    WIND("Wind"),
    TRAVEL("On the road"),
    CITY("City"),
    VEHICLE("Vehicle"),
    TAVERN("Tavern"),
    FIRE("Fire"),
    HOME("Home"),
    ARCHIVE_QUIET("Archive"),
    CAVE("Cave"),
    SPACESHIP("Spaceship"),
    BATTLE("Battle");

    /** Bundled loop for this scene, relative to the assets root. NONE has none. */
    val assetPath: String?
        get() = if (this == NONE) null else "ambience/${name.lowercase()}.ogg"

    companion object {
        /** Case-insensitive lookup; returns null for unknown names instead of throwing. */
        fun parse(value: String?): Scene? {
            if (value.isNullOrBlank()) return null
            val key = value.trim().uppercase().replace(' ', '_').replace('-', '_')
            return entries.firstOrNull { it.name == key }
        }

        /** Every scene that has a sound (everything except NONE). */
        val audible: List<Scene> get() = entries.filter { it != NONE }
    }
}
