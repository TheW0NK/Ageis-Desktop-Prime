# AUI: the Aegis UI language

AUI describes windows as a tree of widgets, written like HTML. AUI has no
styling. Elements name widgets and attributes set their properties. The
theme (light, dark or high contrast) decides how everything looks, so every
app matches the system.

```xml
<window title="Hello" width="360" padding="16" spacing="10">
  <label text="Your name"/>
  <input id="name" placeholder="Type here" onactivate="greet"/>
  <hbox justify="end">
    <button text="Greet" default="true" onclick="greet"/>
  </hbox>
</window>
```

```c
#include "ui.h"

static void greet(struct widget *w, void *user)
{
    struct ui_window *win = ui_window_of(w);
    ui_message(win, "Hello", ui_text(ui_get(win, "name")), "OK");
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = { { "greet", greet }, { NULL, NULL } };

    if (!ui_load("/usr/share/hello/hello.aui", handlers, NULL))
        return 1;
    return ui_run();
}
```

The C API is in `endpoint/lib/include/ui.h`. Programs link with `libgfx.a`.

## Syntax

- `<tag attr="value">children and text</tag>` or `<tag attr='value'/>`.
- An attribute written without a value means `"true"`. Unquoted values are
  also accepted.
- Text inside an element becomes its `text` attribute. White space collapses
  the way it does in HTML. `<br>` starts a new line. Inside `<textarea>` the
  text is kept exactly as written.
- `<!-- comments -->` and `<?xml ...?>` are skipped.
- Supported entities: `&lt; &gt; &amp; &quot; &apos; &nbsp; &copy; &hellip;
  &mdash; &ndash; &bull; &times; &deg; &euro;` and the arrows, plus `&#N;`
  and `&#xN;`.
- Elements that never have children may be left unclosed, as in HTML:
  `<input>`, `<separator>`, `<image>`, `<slider>` and the like.
- HTML names are accepted as aliases: `div`, `row`, `column`, `img`, `hr`,
  `select`, `a`, `span`, `fieldset` and others (listed at the end).

### Translation

A `text`, `title`, `placeholder` or `tooltip` value that starts with `@` is a
translation key: `text="@file.open"`. The key is looked up through
`ui_set_translator()`. Write `@@` for a literal `@`.

## The window

The root element is `<window>`. Its attributes:

| Attribute | Meaning |
|---|---|
| `title` | Title bar text. |
| `width`, `height` | Content size. Leave either out to fit the content. |
| `resizable="false"` | The window has a fixed size. |
| `role` | `normal` (default), `dialog`, `panel`, `desktop`, `overlay`. |
| `dock="top"` | For panels: dock at the top instead of the bottom. |
| `hidden="true"` | Do not show the window when `ui_run()` starts. Use `ui_window_show()` to show it. |
| `modal="true"` | Block input to the app's other windows while this one is open. |
| `onclose` | Handler called when the user closes the window. The window then stays open until the handler calls `ui_window_close()`. |
| `padding`, `spacing` | As for `<vbox>`. Both default to 0, so menu bars and toolbars sit flush with the frame. |

A window lays out its children top to bottom, like `<vbox>`.

## Attributes every element takes

| Attribute | Meaning |
|---|---|
| `id` | Name for `ui_get(win, id)`. |
| `text` | The label, caption or content. |
| `expand` | Share of extra space along the parent's direction (`true` = 1, or a weight such as `2`). |
| `width`, `height` | Fixed size in pixels. |
| `minwidth`, `minheight` | Smallest size. |
| `align` | Placement across the parent's direction: `stretch` (default), `start`, `center`, `end`. |
| `textalign` | For text: `left`, `center`, `right`. |
| `padding`, `spacing` | Containers: inner margin, and the gap between children. |
| `disabled`, `hidden` | Starting state (`ui_set_enabled`, `ui_set_visible`). |
| `value`, `min`, `max`, `step` | Numbers for sliders, progress bars, spin boxes, check boxes and selections. |
| `shortcut` | A key combination that clicks the element, such as `Ctrl+S`, `F5` or `Ctrl+Shift+Z`. |
| `on<event>` | A handler name from the table passed to `ui_load()`. |

## Containers

| Element | Description |
|---|---|
| `vbox` | Children top to bottom. `justify="start\|center\|end"` places the children when none of them expands. |
| `hbox` | Children left to right. Takes `justify` too. |
| `grid` | `columns="N"`. Children fill the grid row by row. `stretch="1,2"` lists the columns that grow (the last column by default). |
| `group` | A framed `vbox` with a `title`. |
| `scroll` | A scrolling `vbox`: wheel, scroll bar, PageUp/PageDown. Focused widgets are scrolled into view. |
| `stack` | Shows the child whose index is `value`. |
| `tabs` | A stack with a row of tab headers. Children are `<tab title="...">`. `onchange` fires when the user switches tabs. Ctrl+Tab cycles through the tabs. |
| `toolbar` | A row with a toolbar background. Usually holds `flat` buttons. |
| `statusbar` | A row along the bottom of the window. |

## Text and images

| Element | Description |
|---|---|
| `label` | One or more lines of text. Attributes: `wrap`, `size` (`small`, `large`, `title`, `huge` or pixels), `bold`, `mono`, `dim`. |
| `h1`, `h2` | Headings. |
| `p` | A label that wraps. |
| `link` | Clickable text. `onclick`. |
| `image` | `src` is a PNG, JPEG, BMP, GIF or TGA file. `scale` is `fit` (shrink to fit, the default), `fill`, `stretch` or `none`. Change it from C with `ui_image_set()`. |
| `separator` | A line: horizontal in a column, vertical in a row. |
| `spacer` | Empty space that expands, or a fixed `size`. |

## Controls

| Element | Events | Description |
|---|---|---|
| `button` | `onclick` | `default="true"` makes it the primary button, which Enter presses. `cancel="true"` makes Escape press it. Also takes `flat`, `icon="path"` and `menu="id"` (opens that menu). |
| `checkbox`, `toggle` | `onchange` | `checked="true"`. `ui_value()` is 0 or 1. |
| `radio` | `onchange` | Radios with the same parent, or the same `group="name"`, exclude each other. |
| `slider` | `onchange`, `onrelease` | `min`, `max`, `step`, `value`. |
| `progress` | | `value` from `min` to `max` (0 to 100 by default). |
| `input` | `onchange`, `onactivate` (Enter) | One line of text. `placeholder`, `readonly`, `maxlength`. |
| `password` | `onchange`, `onactivate` | An `input` that shows bullets and cannot be copied from. |
| `spin` | `onchange` | A number with up and down arrows. Takes `min`, `max`, `step`. Arrow keys and the wheel change it. |
| `textarea` | `onchange` | Multi-line text. `wrap`, `mono`, `readonly`, `autoindent`, `tabfocus` (Tab moves focus instead of typing a tab), `placeholder`. |
| `list` | `onselect`, `onactivate`, `oncontext` | Rows of text. Children are `<item>` elements, which take `icon` and `selected`. Typing a letter jumps to the matching row. `singleclick` activates a row on one click. |
| `table` | as `list`, plus `onsort` | A list with column headers: `columns="Name\|Size:80:right\|Date:140"`. Each column is a title, an optional width and an optional alignment. Rows hold tab-separated values. Clicking a header sets `sortcolumn` and `sortdescending`, then fires `onsort`. |
| `dropdown` | `onchange` | Choose one of its `<option>` children. |
| `canvas` | | Custom drawing and input through `ui_canvas_set()`. Pointer positions are relative to the canvas. |

### Text editing keys

These keys work in `input`, `password`, `spin` and `textarea`:

- Moving: arrows, Home, End, PageUp, PageDown. Ctrl plus an arrow moves by
  words. Ctrl+Home and Ctrl+End go to the start and end. Shift extends the
  selection.
- Mouse: double click selects a word, triple click selects a line.
- Editing: Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+Z, and Ctrl+Y or
  Ctrl+Shift+Z. Ctrl+Backspace and Ctrl+Delete delete a word.

## Menus

```xml
<menubar>
  <menu text="File">
    <item text="Open..." shortcut="Ctrl+O" onclick="open"/>
    <separator/>
    <menu text="Recent">
      <item text="notes.txt" onclick="recent"/>
    </menu>
    <item id="wrap" text="Word wrap" checked="true" onclick="wrap"/>
  </menu>
</menubar>
<menu id="context">
  <item text="Copy" onclick="copy"/>
</menu>
```

- Menus open as popup windows.
- Keyboard: F10 opens the menu bar. The arrow keys, Enter and Escape move
  through the menus, and typing a letter jumps to the matching item.
- An `<item>` takes `shortcut`, `checked` and `disabled`.
- A `<menu>` outside a menu bar is never shown in place. Open it with
  `ui_menu_popup(menu, widget, x, y)`, usually from an `oncontext` handler.
  Pass `x = -1` to open it at the pointer.

## Dialogs

| Function | Returns |
|---|---|
| `ui_message(parent, title, text, "Save\|Don't save\|Cancel")` | The index of the button pressed, or -1. The first button is the default. |
| `ui_prompt(parent, title, text, initial)` | A string you must free, or NULL. |
| `ui_file_dialog(parent, title, start_dir, save, suggested_name)` | A path you must free, or NULL. Shows the user's folders (Home, Desktop, Documents, Downloads, Images, Music) and the whole computer. Asks before replacing a file. `ui_file_dialog_filtered` also takes `"*.txt;*.md"`. |

These functions run their own event loop and return when the user answers.

## Event loop

- `ui_run()` shows the windows and handles events until `ui_quit(code)` or
  the last window closes.
- `ui_timer(ms, fn, user)` calls `fn` repeatedly until it returns false.
- `ui_watch_fd(fd, fn, user)` calls `fn` when `fd` is readable, which suits
  sockets, pipes and pseudo-terminals.

## Keyboard

- Tab and Shift+Tab move the focus.
- Enter presses the `default` button and Escape presses the `cancel` button.
- `shortcut` attributes work anywhere in the window.
- `ui_on_key(win, fn, user)` sees every key after the default handling.

## Themes

- `ui_set_theme("light" | "dark" | "high-contrast")` changes the theme.
- `ui_load_user_theme()` applies the theme from the `AEGIS_THEME`
  environment variable, or from the user's settings file
  `/users/<name>/system/settings/theme`.
- The high-contrast theme also uses larger text.

## Aliases

| Alias | Element |
|---|---|
| `div`, `column`, `col` | `vbox` |
| `row` | `hbox` |
| `img` | `image` |
| `hr` | `separator` |
| `select`, `combobox` | `dropdown` |
| `listbox` | `list` |
| `textbox` | `input` |
| `check` | `checkbox` |
| `switch` | `toggle` |
| `tabview` | `tabs` |
| `page` | `tab` |
| `frame`, `fieldset` | `group` |
| `a` | `link` |
| `span`, `text`, `pre` | `label` |
| `h3` | `h2` |
