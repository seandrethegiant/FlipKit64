# Synido TempoPAD Z-1: what we know about its MIDI protocol

Found by testing and by a USB capture of Synido's Windows editor (Oct 30 2026, `DAW SETUP\tempopad-colours.pcapng`).
USB: VID 28e3, PID 1005. Class-compliant USB MIDI (bulk OUT ep 0x02, IN ep 0x81).

## Pads and buttons (input)
- Drum / Key mode: channel 1 notes 36-99 (Octave shifts them). User mode: channel 3 notes 36-99.
- Easy mode: SysEx `F0 nn F7` per pad, `nn & 0x3F` = pad index (note - 36), `0x40` set = release.
- Custom mode: whatever the editor set (now notes 36-99 on channel 2).
- MMC transport `F0 7F 7F 06 cc F7` (01 stop, 02 play, 06 record) when the MMC button is on.

## Lights: LIVE, in Easy mode (confirmed Oct 30 2026)
`F0 65 (pad R G B)... F7` sets pad colours immediately, nothing saved. Works ONLY while the TempoPAD is in Easy mode
(does nothing in Drum or Custom mode). Colours 0-127 per channel. Pad index = note - 36 in FlipKit64's layout
(0 = bottom-left, 0-3 bottom row of the left half, 4-7 the row above, 32+ = right half). Any number of pads per
message: one pad (7 bytes) or all 64 (259 bytes). The editor always sends all 64. Sending ~660 frames/sec flickers:
limit to about 30 frames/sec. Test log: `DAW SETUP\led-test-results.txt`.
- Notes / CCs sent to the TempoPAD do NOT light pads in any mode (tested every channel, all velocities).

## Synido SysEx (from the capture)
Header `F0 56 00 20 6F`, then a command byte.
- `F0 75 00 20 6F 02 F7` -> device replies `F0 75 00 20 6F 02 01 00 08 F7` (version / identity?).
- `F0 56 00 20 6F 02 F7` = read the Custom-mode setup. Reply: same layout as the write below, command byte 02.
- `F0 56 00 20 6F 01 <data> F7` = write the Custom-mode setup (658 bytes total). Applies immediately in Custom mode
  and survives a PC restart, so it is probably saved to flash: do NOT send it many times a second.

`<data>` = `00 00`, then 64 pad records of 10 bytes in pad order (pad 1 = bottom-left, note 36 in our layout), then 9 bytes
for Stop / Play / Record (3 bytes each, first = key: Stop 7F = CC 127 by default).

Pad record: `tt nn cc  mm  onR onG onB  offR offG offB`
- `tt` 00 = note (CC / PC types not captured yet), `nn` = note number, `cc` = MIDI channel - 1, `mm` = 00 (meaning unknown, maybe trigger mode)
- colours are 0-127 per channel (the editor halves 0-255).
- Captured: pad 1 off colour red = `7F 00 00`, green = `00 7F 01`; blue off = `0C 00 7F`; default on (orange) = `7F 3C 00`.

## Notes
- The Custom-mode setup write IS saved (survived unplugging), so only send it rarely.
- Easy mode pad presses arrive as `F0 nn F7` (see above), so FlipKit64 can run its whole display in Easy mode.
