#pragma once
#include <juce_gui_extra/juce_gui_extra.h>   // ColourSelector
#include <vector>

// ---------------------------------------------------------------------------------------------
// Colour themes for the screen. The TempoPAD's LEDs keep their own colours (see Lights.cpp).
//
// The live palette below is what every paint() reads, so a theme switch shows up on the next
// repaint. Controls that were given a colour once (setColour in a constructor) are re-coloured by
// applyTheme(): it works out which palette colour (and which shade of it) each one came from.
namespace UI
{
    // ---- the live palette (Purple Night until a theme is applied)
    inline juce::Colour bg      { 0xff150f22 };   // window background
    inline juce::Colour panel   { 0xff211836 };   // panels and cards
    inline juce::Colour line    { 0xff3a2c58 };   // outlines and grid lines
    inline juce::Colour text    { 0xfff6f3fb };   // main text
    inline juce::Colour muted   { 0xff9d92b8 };   // secondary text
    inline juce::Colour accent  { 0xffff8c2e };   // buttons that are on, knobs, markers
    inline juce::Colour right   { 0xffa45cff };   // second accent (selections, the right half)
    inline juce::Colour rec     { 0xffff3d5a };   // record
    inline juce::Colour ink     { 0xff120c1c };   // dark text on lit pads and steps

    // Pad zones on screen (4x4 quadrants). The defaults match the controller's drum-mode colours.
    inline juce::Colour zonePurple { 0xffc84dff };   // top left
    inline juce::Colour zoneBlue   { 0xff7389fb };   // top right
    inline juce::Colour zoneCyan   { 0xff1ad9fc };   // bottom left
    inline juce::Colour zoneYellow { 0xfffee150 };   // bottom right

    // Shades worked out from the palette (grids, piano roll).
    inline juce::Colour well()     { return bg.interpolatedWith (panel, 0.5f); }     // sunken areas
    inline juce::Colour wellEdge() { return bg.interpolatedWith (panel, 0.3f); }     // lines between sunken rows
    inline juce::Colour gridLine() { return panel.interpolatedWith (line, 0.15f); }  // minor grid lines
    inline juce::Colour rowLit()   { return panel.interpolatedWith (line, 0.3f); }   // in-scale piano-roll rows
    inline juce::Colour rowRoot()  { return panel.interpolatedWith (line, 0.6f); }   // root-note rows

    struct Theme
    {
        juce::String name;
        juce::Colour bg, panel, line, text, muted, accent, right, rec, ink;
        juce::Colour zonePurple, zoneBlue, zoneCyan, zoneYellow;

        bool sameColours (const Theme&) const;
        juce::String toString() const;                                        // for the settings file
        static Theme fromString (const juce::String&, const Theme& fallback); // missing colours come from fallback
    };

    // The colours a theme is made of, in the order the custom editor lists them.
    struct ThemeSlot { const char* key; const char* label; juce::Colour Theme::* member; };
    const std::vector<ThemeSlot>& themeSlots();

    const std::vector<Theme>& themePresets();   // the first is Purple Night, the default
    const Theme& defaultTheme();
    Theme currentTheme();                       // the live palette
    const Theme* findPreset (const juce::String& name);

    // Switch the live palette and re-colour every FlipKit64 window on screen (and 'extraRoot', for
    // components that aren't on screen yet). The caller re-applies its LookAndFeel colours.
    void applyTheme (const Theme&, juce::Component* extraRoot = nullptr);

    // Re-colour one component tree for the switch 'from' -> 'to' (applyTheme uses this; tests too).
    void recolourTree (juce::Component& root, const Theme& from, const Theme& to);

    // ---- "Custom colours...": pick each colour, with a live preview.
    class ThemeEditor : public juce::Component,
                        private juce::ChangeListener
    {
    public:
        // onChange gets the edited theme every time a colour moves.
        ThemeEditor (Theme start, std::function<void (const Theme&)> onChange);
        ~ThemeEditor() override;
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        const Theme& getTheme() const { return theme; }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override;
        void selectSlot (int i);
        void startFrom (const Theme&);
        juce::Rectangle<int> rowBounds (int i) const;

        Theme theme;
        std::function<void (const Theme&)> onChange;
        int selected = 0;
        juce::Rectangle<int> listArea;
        juce::ColourSelector selector { juce::ColourSelector::showColourspace | juce::ColourSelector::showSliders
                                        | juce::ColourSelector::showColourAtTop | juce::ColourSelector::editableColour };
        juce::ComboBox startBox;   // "Start from a preset"
        juce::Label title, hint;
        bool updating = false;
    };
}
