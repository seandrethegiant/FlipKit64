#include "Theme.h"
#include <set>

using namespace juce;

namespace UI
{
// ===================================================================================== presets

static Theme make (const char* name, uint32 bg_, uint32 panel_, uint32 line_, uint32 text_, uint32 muted_,
                   uint32 accent_, uint32 right_, uint32 rec_, uint32 ink_ = 0xff120c1c)
{
    Theme t;
    t.name = name;
    t.bg = Colour (bg_); t.panel = Colour (panel_); t.line = Colour (line_);
    t.text = Colour (text_); t.muted = Colour (muted_);
    t.accent = Colour (accent_); t.right = Colour (right_); t.rec = Colour (rec_); t.ink = Colour (ink_);
    // Pads on screen match the TempoPAD's drum-mode colours in every preset (Custom can change them).
    t.zonePurple = Colour (0xffc84dff); t.zoneBlue = Colour (0xff7389fb);
    t.zoneCyan = Colour (0xff1ad9fc);   t.zoneYellow = Colour (0xfffee150);
    return t;
}

const std::vector<Theme>& themePresets()
{
    // Note: pure black/white are avoided for palette colours, so they can't be mistaken for the
    // fixed black/white some controls use (see recolourTree).
    static const std::vector<Theme> presets {
        make ("Purple Night",  0xff150f22, 0xff211836, 0xff3a2c58, 0xfff6f3fb, 0xff9d92b8, 0xffff8c2e, 0xffa45cff, 0xffff3d5a),
        make ("MPC Classic",   0xff1b1b1d, 0xff26272a, 0xff3e4045, 0xfff2f2ee, 0xffa4a5a9, 0xffe8a33d, 0xffd9534f, 0xffff3b30, 0xff141414),
        make ("Midnight Blue", 0xff0d1321, 0xff16203a, 0xff2a3b63, 0xffeef3fc, 0xff8e9dbd, 0xff2ec5ff, 0xff8b7bff, 0xffff4d6a, 0xff0a1020),
        make ("Forest",        0xff0e1511, 0xff17231c, 0xff2b4234, 0xffeef6f0, 0xff93ab9c, 0xff8ee26b, 0xffffb84d, 0xffff5a4f, 0xff0c140f),
        make ("Daylight",      0xffe6e2ee, 0xfff4f2f8, 0xffc9c1d9, 0xff1e1830, 0xff645b7a, 0xffe5661c, 0xff7b3fe0, 0xffe0263f),
        make ("High Contrast", 0xff050505, 0xff121212, 0xff6e6e6e, 0xfffefefe, 0xffd2d2d2, 0xffffd400, 0xff00e5ff, 0xffff2d55, 0xff0c0c0c),
    };
    return presets;
}

const Theme& defaultTheme() { return themePresets().front(); }

const Theme* findPreset (const String& name)
{
    for (auto& t : themePresets())
        if (t.name == name) return &t;
    return nullptr;
}

const std::vector<ThemeSlot>& themeSlots()
{
    static const std::vector<ThemeSlot> slots {
        { "bg",     "Background",           &Theme::bg },
        { "panel",  "Panels",               &Theme::panel },
        { "line",   "Lines",                &Theme::line },
        { "text",   "Text",                 &Theme::text },
        { "muted",  "Dim text",             &Theme::muted },
        { "accent", "Accent",               &Theme::accent },
        { "right",  "Second accent",        &Theme::right },
        { "rec",    "Record",               &Theme::rec },
        { "ink",    "Text on lit pads",     &Theme::ink },
        { "zoneTL", "Pads: top left",       &Theme::zonePurple },
        { "zoneTR", "Pads: top right",      &Theme::zoneBlue },
        { "zoneBL", "Pads: bottom left",    &Theme::zoneCyan },
        { "zoneBR", "Pads: bottom right",   &Theme::zoneYellow },
    };
    return slots;
}

// ===================================================================================== Theme

bool Theme::sameColours (const Theme& o) const
{
    for (auto& s : themeSlots())
        if (this->*s.member != o.*s.member) return false;
    return true;
}

String Theme::toString() const
{
    StringArray parts;
    parts.add ("name=" + name.removeCharacters (";="));
    for (auto& s : themeSlots()) parts.add (String (s.key) + "=" + (this->*s.member).toString());
    return parts.joinIntoString (";");
}

Theme Theme::fromString (const String& str, const Theme& fallback)
{
    Theme t = fallback;
    StringArray parts;
    parts.addTokens (str, ";", "");
    for (auto& p : parts)
    {
        const auto key = p.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto val = p.fromFirstOccurrenceOf ("=", false, false).trim();
        if (key == "name") { if (val.isNotEmpty()) t.name = val; continue; }
        for (auto& s : themeSlots())
            if (key == s.key && val.containsOnly ("0123456789abcdefABCDEF") && val.length() == 8)
                t.*s.member = Colour::fromString (val);
    }
    return t;
}

// ===================================================================================== live palette

static String& currentName() { static String n ("Purple Night"); return n; }

Theme currentTheme()
{
    Theme t;
    t.name = currentName();
    t.bg = bg; t.panel = panel; t.line = line; t.text = text; t.muted = muted;
    t.accent = accent; t.right = right; t.rec = rec; t.ink = ink;
    t.zonePurple = zonePurple; t.zoneBlue = zoneBlue; t.zoneCyan = zoneCyan; t.zoneYellow = zoneYellow;
    return t;
}

static void setPalette (const Theme& t)
{
    currentName() = t.name;
    bg = t.bg; panel = t.panel; line = t.line; text = t.text; muted = t.muted;
    accent = t.accent; right = t.right; rec = t.rec; ink = t.ink;
    zonePurple = t.zonePurple; zoneBlue = t.zoneBlue; zoneCyan = t.zoneCyan; zoneYellow = t.zoneYellow;
}

// ===================================================================================== re-colouring

namespace
{
    // The shades the UI code makes from palette colours (colour.brighter (0.08f) and so on).
    struct Shade { char kind; float amount; };   // '=' as is, '+' brighter, '-' darker, 'a' alpha
    const Shade shades[] = {
        { '=', 0.0f },
        { '+', 0.03f }, { '+', 0.04f }, { '+', 0.05f }, { '+', 0.06f }, { '+', 0.07f }, { '+', 0.08f },
        { '+', 0.12f }, { '+', 0.15f }, { '+', 0.25f },
        { '-', 0.2f }, { '-', 0.3f }, { '-', 0.35f },
        { 'a', 0.22f }, { 'a', 0.25f }, { 'a', 0.35f }, { 'a', 0.4f }, { 'a', 0.5f }, { 'a', 0.55f },
        { 'a', 0.6f }, { 'a', 0.75f }, { 'a', 0.85f },
    };
    constexpr int numShades = (int) (sizeof (shades) / sizeof (shades[0]));

    Colour shade (Colour c, const Shade& s)
    {
        switch (s.kind)
        {
            case '+': return c.brighter (s.amount);
            case '-': return c.darker (s.amount);
            case 'a': return c.withAlpha (s.amount);
            default:  return c;
        }
    }

    // Which palette colour + shade gives this colour in theme t: slot * 64 + shade, or -1.
    int findCode (const Theme& t, Colour c)
    {
        // Fixed colours some controls use on purpose (black text on the accent, clear outlines).
        if (c.getAlpha() == 0 || c == Colours::black || c == Colours::white) return -1;
        const auto& slots = themeSlots();
        for (int i = 0; i < (int) slots.size(); ++i)
            for (int k = 0; k < numShades; ++k)
                if (shade (t.*slots[(size_t) i].member, shades[k]) == c) return i * 64 + k;
        return -1;
    }

    Colour fromCode (const Theme& t, int code)
    {
        return shade (t.*themeSlots()[(size_t) (code / 64)].member, shades[code % 64]);
    }
}

void recolourTree (Component& root, const Theme& from, const Theme& to)
{
    // Each control colour is tagged with the palette colour it came from ("fkthm_<id>" = code:argb),
    // so later switches don't depend on colours matching up again. If the code set a new colour
    // since (argb differs), the tag is worked out afresh from the 'from' palette.
    static const String colourPrefix ("jcclr_"), tagPrefix ("fkthm_");

    auto& props = root.getProperties();
    StringArray names;
    for (int i = 0; i < props.size(); ++i)
        if (props.getName (i).toString().startsWith (colourPrefix)) names.add (props.getName (i).toString());

    for (auto& n : names)
    {
        const auto hex = n.substring (colourPrefix.length());
        const int colourId = hex.getHexValue32();
        const auto argb = (uint32) (int) props[Identifier (n)];
        const Identifier tagId (tagPrefix + hex);

        int code = -1;
        bool known = false;
        if (props.contains (tagId))
        {
            const auto tag = props[tagId].toString();
            if ((uint32) tag.fromFirstOccurrenceOf (":", false, false).getHexValue32() == argb)
            {
                code = tag.upToFirstOccurrenceOf (":", false, false).getIntValue();
                known = true;
            }
        }
        if (! known) code = findCode (from, Colour (argb));

        if (code >= 0)
        {
            const auto c = fromCode (to, code);
            if (c.getARGB() != argb) root.setColour (colourId, c);
            props.set (tagId, String (code) + ":" + String::toHexString ((int) c.getARGB()));
        }
        else props.set (tagId, "-1:" + String::toHexString ((int) argb));
    }

    for (auto* child : root.getChildren())
        recolourTree (*child, from, to);
}

void applyTheme (const Theme& t, Component* extraRoot)
{
    const Theme from = currentTheme();
    setPalette (t);

    std::set<Component*> done;
    auto run = [&] (Component* c)
    {
        if (c == nullptr || ! done.insert (c).second) return;
        recolourTree (*c, from, t);
        c->repaint();
    };
    auto& desktop = Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i) run (desktop.getComponent (i));
    if (extraRoot != nullptr) run (extraRoot->getTopLevelComponent());
}

// ===================================================================================== ThemeEditor

static constexpr int rowH = 22, listW = 210;

ThemeEditor::ThemeEditor (Theme start, std::function<void (const Theme&)> changed)
    : theme (std::move (start)), onChange (std::move (changed))
{
    title.setText ("Custom colours", dontSendNotification);
    title.setFont (Font (14.0f, Font::bold));
    title.setColour (Label::textColourId, text);
    addAndMakeVisible (title);

    hint.setText ("Pick a colour on the left, then change it on the right. Changes show straight away "
                  "and are saved. The TempoPAD's lights keep their own colours.", dontSendNotification);
    hint.setFont (Font (11.0f));
    hint.setColour (Label::textColourId, muted);
    hint.setJustificationType (Justification::topLeft);
    addAndMakeVisible (hint);

    startBox.setTextWhenNothingSelected ("Start from a preset...");
    int id = 1;
    for (auto& p : themePresets()) startBox.addItem (p.name, id++);
    startBox.onChange = [this]
    {
        const int i = startBox.getSelectedId() - 1;
        if (i >= 0 && i < (int) themePresets().size()) startFrom (themePresets()[(size_t) i]);
        startBox.setSelectedId (0, dontSendNotification);
    };
    addAndMakeVisible (startBox);

    selector.setColour (ColourSelector::backgroundColourId, Colours::transparentBlack);
    selector.addChangeListener (this);
    addAndMakeVisible (selector);

    selectSlot (0);
    setSize (listW + 300, 40 + (int) themeSlots().size() * rowH + 56);
}

ThemeEditor::~ThemeEditor() { selector.removeChangeListener (this); }

Rectangle<int> ThemeEditor::rowBounds (int i) const { return { listArea.getX(), listArea.getY() + i * rowH, listArea.getWidth(), rowH }; }

void ThemeEditor::paint (Graphics& g)
{
    const auto& slots = themeSlots();
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        auto r = rowBounds (i);
        if (i == selected)
        {
            g.setColour (accent.withAlpha (0.25f));
            g.fillRoundedRectangle (r.toFloat().reduced (1.0f), 4.0f);
        }
        auto sw = r.removeFromLeft (rowH).reduced (3).toFloat();
        g.setColour (theme.*slots[(size_t) i].member);
        g.fillRoundedRectangle (sw, 3.0f);
        g.setColour (text.withAlpha (0.35f));
        g.drawRoundedRectangle (sw, 3.0f, 1.0f);
        g.setColour (i == selected ? text : muted);
        g.setFont (Font (12.0f));
        g.drawText (slots[(size_t) i].label, r.withTrimmedLeft (6), Justification::centredLeft);
    }
}

void ThemeEditor::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto top = r.removeFromTop (26);
    startBox.setBounds (top.removeFromRight (180));
    title.setBounds (top);
    r.removeFromTop (6);
    hint.setBounds (r.removeFromBottom (44));
    listArea = r.removeFromLeft (listW - 8).withHeight ((int) themeSlots().size() * rowH);
    r.removeFromLeft (8);
    selector.setBounds (r);
}

void ThemeEditor::mouseDown (const MouseEvent& e)
{
    for (int i = 0; i < (int) themeSlots().size(); ++i)
        if (rowBounds (i).contains (e.getPosition())) { selectSlot (i); return; }
}

void ThemeEditor::selectSlot (int i)
{
    selected = i;
    updating = true;
    selector.setCurrentColour (theme.*themeSlots()[(size_t) i].member, dontSendNotification);
    updating = false;
    repaint();
}

void ThemeEditor::startFrom (const Theme& preset)
{
    theme = preset;
    theme.name = "Custom";
    selectSlot (selected);
    if (onChange) onChange (theme);
}

void ThemeEditor::changeListenerCallback (ChangeBroadcaster*)
{
    if (updating) return;
    const auto c = selector.getCurrentColour();
    auto& slot = theme.*themeSlots()[(size_t) selected].member;
    if (c == slot) return;
    slot = c;
    theme.name = "Custom";
    repaint();
    if (onChange) onChange (theme);
}
}
