// Browser test of the PicoGUS web page against the mock card (mock_card.py):
// everything is done once with the keyboard only and once with the mouse only.
//
//   node ui_test.js [screenshot dir]
//
// Needs Node.js with Playwright and its Chromium (npm i playwright), and
// Python 3 for the mock card, which this script starts itself.
const { chromium } = require('playwright');
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const PORT = 8771, BASE = 'http://127.0.0.1:' + PORT;
const shots = process.argv[2];
let failures = 0;
const check = (ok, what) => { if (!ok) { failures++; console.log('FAIL: ' + what); } };

async function main() {
  const card = spawn('python3', [path.join(__dirname, 'mock_card.py'), String(PORT)], { stdio: 'ignore' });
  await new Promise(r => setTimeout(r, 800));
  const browser = await chromium.launch();
  try {
    await run(browser);
  } catch (e) {
    failures++;
    console.log('FAIL: test stopped: ' + String(e.stack || e).split('\n').slice(0, 6).join(' / '));
  } finally {
    await browser.close();
    card.kill();
  }
  console.log(failures ? 'FAILED: ' + failures + ' failure(s)' : 'OK: 0 failure(s)');
  process.exit(failures ? 1 : 0);
}

async function open(browser, width, height) {
  const page = await (await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1 })).newPage();
  page.errors = [];
  page.on('console', m => { if (m.type() === 'error') page.errors.push(m.text()); });
  page.on('pageerror', e => page.errors.push(String(e)));
  await page.goto(BASE + '/');
  await page.waitForSelector('.dos-menu__item');
  return page;
}
const title = page => page.evaluate(() => { const w = [...document.querySelectorAll('.win:not([hidden]):not([inert]) .dos-window__title')]; return w.length ? w[0].textContent : ''; });
const bar = page => page.evaluate(() => document.activeElement ? document.activeElement.textContent.replace('►', '').trim() : '');
const desc = page => page.textContent('#desc');
const log = async page => (await page.request.get(BASE + '/log')).json();
const last = async page => { const l = await log(page); return l[l.length - 1]; };
const keys = async (page, ...ks) => { for (const k of ks) await page.keyboard.press(k); };
const shot = async (page, name) => { if (shots) await page.screenshot({ path: path.join(shots, name + '.png') }); };
const settle = page => page.waitForTimeout(250);
// the card has restarted and the main menu is back
const ready = page => page.waitForFunction(() => document.getElementById('desc').textContent.startsWith('The card is ready')
  && !document.querySelector('.win[inert]'), null, { timeout: 20000 });
// a command or button of the window that takes input
const cmd = text => '.win:not([hidden]):not([inert]) :is(button,label):has-text("' + text + '")';

// Every window must sit on whole character cells, inside the screen
async function grid(page, where) {
  const bad = await page.evaluate(() => {
    const body = document.getElementById('body').getBoundingClientRect(), out = [];
    const p = document.createElement('span');
    p.style.cssText = 'position:absolute;visibility:hidden;width:10ch;height:1em';
    document.getElementById('dos').append(p);
    const cw = p.getBoundingClientRect().width / 10, ch = p.getBoundingClientRect().height;
    p.remove();
    const off = (v, cell) => Math.abs(v / cell - Math.round(v / cell)) > 0.02;
    for (const w of document.querySelectorAll('.win:not([hidden])')) {
      const r = w.getBoundingClientRect(), name = w.querySelector('.dos-window__title').textContent;
      if (off(r.left - body.left, cw) || off(r.top - body.top, ch) || off(r.width, cw) || off(r.height, ch)) out.push(name + ' is off the grid');
      if (r.left < body.left - 1 || r.right > body.right + 1 || r.top < body.top - 1 || r.bottom > body.bottom + 1) out.push(name + ' leaves the screen');
      for (const e of w.querySelectorAll('.dos-window__title,.dos-button,.dos-field,.dos-choice,.dos-label,.dos-progress')) {
        const q = e.getBoundingClientRect();
        if (q.width && off(q.left - body.left, cw)) out.push(name + ': ' + (e.textContent || e.className).slice(0, 20) + ' is between two cells');
        if (q.right > r.right + 1) out.push(name + ': ' + (e.textContent || e.className).slice(0, 20) + ' sticks out');
      }
    }
    const bars = document.querySelectorAll('.win:not([inert]) .is-selected, .win:not([inert]) .dos-button.is-focused, .win:not([inert]) .dos-choice.is-focused');
    if (bars.length > 1) out.push(bars.length + ' bars at once');
    const st = document.getElementById('bar');
    if (st.scrollWidth > st.clientWidth + 1) out.push('status line overflows');
    return out;
  });
  check(!bad.length, where + ': ' + bad.join('; '));
}

async function run(browser) {
  // ---- keyboard only -------------------------------------------------------
  let page = await open(browser, 1500, 850);
  check((await page.getAttribute('#dos', 'class')).includes('screen-2x'), 'a 1500x850 window shows the screen at 2x');
  check(await bar(page) === 'Volume', 'the bar starts on the first command: ' + await bar(page));
  check((await desc(page)).startsWith('Set the volume'), 'status line describes the command: ' + await desc(page));
  await grid(page, 'main menu');
  await shot(page, 'main');

  // arrows move the bar and wrap
  await keys(page, 'ArrowDown', 'ArrowDown');
  check(await bar(page) === 'Settings', 'two down is Settings: ' + await bar(page));
  await keys(page, 'ArrowUp', 'ArrowUp', 'ArrowUp');
  check(await bar(page) === 'Restart Card', 'up from the top wraps to the last command: ' + await bar(page));
  await keys(page, 'Home');
  check(await bar(page) === 'Volume', 'Home goes to the first command');

  // volume: Enter opens, arrows adjust, Esc closes
  await keys(page, 'Enter');
  check(await title(page) === 'Volume', 'Enter opens Volume: ' + await title(page));
  await grid(page, 'volume');
  await keys(page, 'ArrowDown', 'ArrowLeft', 'ArrowLeft', 'ArrowLeft');
  await page.waitForTimeout(400);
  check(JSON.stringify((await last(page)).form) === JSON.stringify({ sbvol: '84' }), 'left three times: SB volume 90 -> 84: ' + JSON.stringify(await last(page)));
  await keys(page, 'End');
  await page.waitForTimeout(400);
  check((await last(page)).form.sbvol === '100', 'End sets 100');
  await shot(page, 'volume');
  await keys(page, 'Escape');
  check(await title(page) === 'Main Menu' && await bar(page) === 'Volume', 'Esc returns to the menu, bar where it was');

  // settings by hotkeys: S(ettings) S(ound Blaster)
  await keys(page, 's');
  check(await title(page) === 'Settings', 'hotkey S opens Settings: ' + await title(page));
  await grid(page, 'settings menu');
  await keys(page, 's');
  check(await title(page) === 'Sound Blaster', 'hotkey S opens Sound Blaster: ' + await title(page));
  await grid(page, 'sound blaster');
  await shot(page, 'settings');
  // port field has the focus with its text selected: typing replaces it
  await page.keyboard.type('240');
  await keys(page, 'ArrowDown', 'ArrowRight');           // IRQ 5 -> 7
  await keys(page, 'Tab', 'Tab', 'Tab', 'Tab');          // DMA, type, lock mixer, fix time constant
  await keys(page, 'Space');                             // check it
  await keys(page, 'Enter');                             // OK
  await settle(page);
  check(JSON.stringify((await last(page)).form) === JSON.stringify({ sbport: '576', sbirq: '7', sbfixtc: '1' }),
        'settings applied in one request: ' + JSON.stringify(await last(page)));
  check(await title(page) === 'Settings', 'OK closes the dialog: ' + await title(page));
  check((await desc(page)).startsWith('Applied'), 'status line confirms: ' + await desc(page));

  // a bad entry is refused with an error box, the dialog stays
  await keys(page, 's');
  await page.keyboard.type('zz');
  await keys(page, 'Enter');
  check(await title(page) === 'Error', 'bad port gives an error box: ' + await title(page));
  await grid(page, 'error box');
  await shot(page, 'error');
  await keys(page, 'Enter');
  check(await title(page) === 'Sound Blaster', 'the dialog is still open');
  // Alt+letter is the hotkey while typing: Alt+C = Cancel
  await keys(page, 'Alt+c');
  check(await title(page) === 'Settings', 'Alt+C cancels: ' + await title(page));
  await keys(page, 'Escape');

  // card mode: M, G(ravis Ultrasound), confirm with Enter
  await keys(page, 'm');
  check(await title(page) === 'Card Mode' && await bar(page) === 'Sound Blaster√', 'mode menu, bar on the current mode: ' + await bar(page));
  await grid(page, 'card mode');
  await keys(page, 'g');
  check(await title(page) === 'Card Mode' && await bar(page) === '[< Yes >]'.replace(/[\[\]<> ]/g, '') , 'confirm box with Yes as default: ' + await bar(page));
  await grid(page, 'mode confirm');
  await shot(page, 'mode');
  await keys(page, 'Enter');
  await ready(page);
  check(JSON.stringify((await last(page)).form) === JSON.stringify({ mode: '1', save: '1' }), 'mode request: ' + JSON.stringify(await last(page)));
  check((await page.textContent('.dos-window--single')).includes('Gravis Ultrasound'), 'status box shows the new mode');

  // MIDI: I, then D(isplay Text), type, Enter
  await keys(page, 'i');
  check(await title(page) === 'MIDI and mt32-pi', 'hotkey I opens MIDI: ' + await title(page));
  await grid(page, 'midi menu');
  await shot(page, 'midi');
  await keys(page, 'd');
  await page.keyboard.type('Hello from PicoGUS!');
  await keys(page, 'Enter');
  await settle(page);
  check((await last(page)).form.hex === 'f04110161220000048656c6c6f2066726f6d205069636f47555321203df7', 'display text sysex: ' + JSON.stringify(await last(page)));
  await keys(page, 'm');      // Use MT-32 Synth
  await settle(page);
  check((await last(page)).form.hex === 'F07D0300F7', 'MT-32 synth command');
  await keys(page, 'r', 'c'); // ROM set > CM-32L
  await settle(page);
  check((await last(page)).form.hex === 'F07D0102F7', 'ROM set submenu command: ' + JSON.stringify(await last(page)));
  await keys(page, 'ArrowLeft');
  check(await title(page) === 'MIDI and mt32-pi', 'left arrow leaves a submenu');
  await keys(page, 'Escape');

  // CD image: C, list scrolls with the bar, Enter loads
  await keys(page, 'c');
  await page.waitForSelector('.dos-listbox__item input:not(:disabled)');
  await settle(page);
  await grid(page, 'cd image');
  await keys(page, 'PageDown', 'ArrowDown', 'ArrowDown');
  check(await page.evaluate(() => { const c = document.querySelector('.dos-listbox input:checked'), l = c.closest('.dos-listbox__items').getBoundingClientRect(), r = c.closest('li').getBoundingClientRect(); return r.top >= l.top - 1 && r.bottom <= l.bottom + 1; }), 'the chosen row is scrolled into view');
  await shot(page, 'cd');
  await keys(page, 'Enter');
  await settle(page);
  check(JSON.stringify((await last(page)).form) === JSON.stringify({ index: '12' }), 'load image 12: ' + JSON.stringify(await last(page)));

  // save with F2, help with F1
  await keys(page, 'F2');
  check(await title(page) === 'Save Settings', 'F2 asks to save: ' + await title(page));
  await keys(page, 'ArrowRight');
  check(await bar(page) === 'No', 'right arrow moves between buttons: ' + await bar(page));
  await keys(page, 'y');
  await settle(page);
  check((await last(page)).path === '/api/save', 'Y saves');
  await keys(page, 'F1');
  check(await title(page) === 'Help', 'F1 shows help');
  await grid(page, 'help');
  await shot(page, 'help');
  await keys(page, 'Escape');

  // display: theme by keyboard
  await keys(page, 'd', 'ArrowRight');
  check(await page.getAttribute('html', 'data-theme') === 'commander', 'right arrow picks the Commander colours');
  await shot(page, 'commander');
  await keys(page, 'ArrowRight');
  await shot(page, 'mono');
  await keys(page, 'ArrowRight');
  check(await page.getAttribute('html', 'data-theme') === 'setup', 'and wraps back to Setup');
  await keys(page, 'Escape');

  // firmware and information windows
  await keys(page, 'f');
  await grid(page, 'firmware');
  await shot(page, 'firmware');
  await keys(page, 'Escape', 'n');
  await grid(page, 'information');
  await shot(page, 'information');
  await keys(page, 'Enter', 'w');
  await grid(page, 'wifi');
  await shot(page, 'wifi');
  await keys(page, 'Escape');
  check(!page.errors.length, 'no script errors (keyboard): ' + page.errors.join(' | '));
  await page.close();

  // ---- mouse only ----------------------------------------------------------
  page = await open(browser, 760, 440);
  check((await page.getAttribute('#dos', 'class')).includes('screen-1x'), 'a small window shows the screen at 1x');
  await page.hover(cmd('Settings'));
  check(await bar(page) === 'Settings', 'the bar follows the pointer: ' + await bar(page));
  await grid(page, 'main menu (mouse)');
  await page.click(cmd('Volume'));
  check(await title(page) === 'Volume', 'click opens Volume');
  const s = await page.locator('.slider').nth(1).boundingBox();
  await page.mouse.click(s.x + s.width * 0.4, s.y + 8);
  await page.waitForTimeout(400);
  check((await last(page)).form.sbvol === '40', 'click on the bar sets the volume: ' + JSON.stringify(await last(page)));
  await page.mouse.move(s.x + s.width * 0.4, s.y + 8);
  await page.mouse.down();
  await page.mouse.move(s.x + s.width * 0.7, s.y + 8, { steps: 5 });
  await page.mouse.up();
  await page.waitForTimeout(400);
  check((await last(page)).form.sbvol === '70', 'dragging moves it: ' + JSON.stringify(await last(page)));
  await page.click(cmd('Close'));
  await page.click(cmd('Settings'));
  await page.click(cmd('MPU-401'));
  await grid(page, 'mpu-401 (mouse)');
  await page.click(cmd('Fake all notes off'));
  await page.fill('.dos-field', '300');
  await page.click(cmd('OK'));
  await settle(page);
  check(JSON.stringify((await last(page)).form) === JSON.stringify({ mpuport: '768', mpufake: '1' }), 'mouse settings: ' + JSON.stringify(await last(page)));
  // status line hints are buttons
  await page.click('#bar >> text=Esc');
  check(await title(page) === 'Main Menu', 'clicking Esc=Back goes back: ' + await title(page));
  // SysEx file and firmware upload
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'pgui-'));
  fs.writeFileSync(path.join(dir, 'test.syx'), Buffer.from([0xf0, 0x41, 0x10, 0x16, 0x12, 0x7f, 0, 0, 0, 1, 0xf7]));
  fs.writeFileSync(path.join(dir, 'picogusw.uf2'), Buffer.concat([Buffer.from('UF2\n'), Buffer.alloc(1020, 1)]));
  await page.click(cmd('MIDI and mt32-pi'));
  let [chooser] = await Promise.all([page.waitForEvent('filechooser'), page.click(cmd('Send SysEx File'))]);
  await chooser.setFiles(path.join(dir, 'test.syx'));
  await page.waitForSelector('text=1 SysEx message sent');
  await grid(page, 'sysex result');
  await page.click(cmd('OK'));
  await page.click('#bar >> text=Esc');
  await page.click(cmd('Firmware Update'));
  [chooser] = await Promise.all([page.waitForEvent('filechooser'), page.click(cmd('Browse'))]);
  await chooser.setFiles(path.join(dir, 'picogusw.uf2'));
  check(await page.inputValue('.dos-field') === 'PICOGUSW.UF2', 'chosen file is shown: ' + await page.inputValue('.dos-field'));
  await page.click(cmd('Install'));
  await page.click(cmd('Yes'));
  await ready(page);
  check((await log(page)).some(l => l.path === '/api/upload/firmware' && l.bytes === 1024), 'firmware uploaded');
  check((await desc(page)).startsWith('The card is ready'), 'status line after the restart: ' + await desc(page));
  // CD: click a row, mouse wheel scrolls, double click loads
  await page.click(cmd('CD-ROM Image'));
  await page.waitForSelector('.dos-listbox__item input:not(:disabled)');
  await page.hover('.dos-listbox__items');
  await page.mouse.wheel(0, 300);
  await page.waitForTimeout(100);
  check(await page.evaluate(() => document.querySelector('.dos-listbox__items').scrollTop % 16 === 0 && document.querySelector('.dos-listbox__items').scrollTop > 0), 'wheel scrolls the list by whole rows');
  await page.dblclick(cmd('GAME05.ISO'));
  await settle(page);
  check(JSON.stringify((await last(page)).form) === JSON.stringify({ index: '8' }), 'double click loads: ' + JSON.stringify(await last(page)));
  // names from the USB drive are shown as text, never as markup
  await page.click(cmd('CD-ROM Image'));
  await page.waitForSelector('.dos-listbox__item input:not(:disabled)');
  check(await page.locator('.dos-listbox b').count() === 0 && await page.locator('text=<b>WC3 & co</b>.CUE').count() === 1, 'file names are not treated as HTML');
  check(!page.errors.length, 'no script errors (mouse): ' + page.errors.join(' | '));
  await page.close();

  // ---- phone ---------------------------------------------------------------
  const phone = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true, deviceScaleFactor: 3 });
  page = await phone.newPage();
  await page.goto(BASE + '/');
  await page.waitForSelector('.dos-menu__item');
  check(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1), 'a phone shows the whole width of the screen');
  await page.tap(cmd('Card Information'));
  check(await title(page) === 'Card Information', 'tapping works on a phone: ' + await title(page));
  await shot(page, 'phone');
  await page.close();
}

main();
