# Textmode

The design system of the PicoGUS web page: the 80x25 colour text screens of
DOS-era sound-card setup programs, in HTML and CSS only.

These files are copied unchanged from the Textmode design system; change them
there and copy them again, not here:

| File | |
| --- | --- |
| `tokens.css` | the 16 VGA colours, the semantic colours of the three themes (`setup`, `commander`, `mono`), cell sizes, the font face |
| `components/bundle.css` | the `.dos-*` components: screen, title and status bars, windows, menus, buttons, fields, choices, list box, progress bar |
| `fonts/Web437_IBM_VGA_9x16.woff` | the IBM VGA 9x16 screen font |

How the page uses them is in `../app.css` and `../app.js`: everything sits on
9x16 pixel character cells, is sized in cells (`1ch` wide, `1em` tall) and uses
the semantic colour tokens, so the theme switch keeps working. The system's
rules for wording (Title Case commands, one hotkey letter per command,
`Key=Action` hints, hardware values as `220h`, `IRQ 5`) apply to every text on
the page.

`scripts/web_pack.py` puts the three files into the page when the firmware is
built, so the card serves one document.

## Font licence

IBM VGA 9x16 comes from *The Ultimate Oldschool PC Font Pack* by VileR
(https://int10h.org/oldschool-pc-fonts/), licensed CC BY-SA 4.0: see
`fonts/LICENSE-oldschool-pc-font-pack.txt`. The page credits it in its Help
window (F1); keep that credit wherever the font is shipped.
