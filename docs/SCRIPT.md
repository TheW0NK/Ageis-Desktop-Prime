# AegisScript

AegisScript is a small language for making apps on Aegis itself. Aegis has
no C compiler, so App Maker builds apps from an AUI window (see
[AUI.md](AUI.md)) plus a script. `/sysapps/apprun` runs them:

```
apprun script.as [ARG...]    # a script; print() writes to the terminal
apprun folder                # an app: folder/app.aui and folder/app.as
```

The syntax looks like JavaScript. Values are numbers, strings, booleans,
`nil`, lists, maps, functions and widgets.

```js
// Comments look like this, or /* like this */.
let name = "Ada";
let scores = [3, 1, 2];
let person = {name: "Grace", year: 1906};

fn greet(who) {
    return "Hello, " + who + "!";
}

if (len(scores) > 2 && !has(person, "age")) {
    print(greet(name));
} else {
    print("no");
}

for (s in scores) { print(s); }      // also: for (i in 10), for (k in map), for (ch in "text")
while (true) { break; }

let add = fn(a, b) { return a + b; };    // functions are values; inner functions keep their variables
```

## Language

**Variables.** `let` declares a variable in the current block. Assigning
with `=`, `+=`, `-=`, `*=`, `/=` or `%=` changes an existing variable.

**Operators.** By precedence, from loosest to tightest:

| Precedence | Operators |
|---|---|
| 1 | `||` |
| 2 | `&&` |
| 3 | `==` `!=` |
| 4 | `<` `<=` `>` `>=` |
| 5 | `+` `-` |
| 6 | `*` `/` `%` |
| 7 (prefix) | `!` `-` |

`+` joins strings, and also joins a string with anything else. `*` repeats
a string.

**Truth.** `false`, `nil`, `0`, `""` and `[]` count as false. Everything
else counts as true.

**Indexing.** `list[i]` (negative indices count from the end),
`map["key"]` or `map.key`, and `text[i]` for one character. `.length`
gives the length of a string or a list.

**Errors.** An error stops the script and reports the file and line. In an
app, an error in a handler is shown in a message, and the app keeps running.

## Built-in functions

| Area | Functions |
|---|---|
| General | `print(...)`, `len(x)`, `str(x)`, `num(text)`, `int(x)`, `type(x)` |
| Lists | `push(list, x...)`, `pop(list)`, `insert(list, i, x)`, `remove(list, i)`, `sort(list)`, `reverse(list)`, `range([from,] to [, step])`, `has(list, x)` |
| Maps | `keys(map)`, `has(map, key)` |
| Strings | `split(text, sep)`, `join(list, sep)`, `upper`, `lower`, `trim`, `find(text, part)`, `replace(text, a, b)`, `substr(text, start [, count])`, `starts(text, prefix)`, `format(number, decimals)` |
| Numbers | `floor`, `ceil`, `round`, `abs`, `sqrt`, `pow`, `sin`, `cos`, `tan`, `log`, `exp`, `min(...)`, `max(...)`, `random()`, `random(n)`, `random(a, b)`, `PI` |
| Time | `now()` (seconds), `clock()` (milliseconds since start), `date([format [, time]])` (strftime formats) |
| Files and programs | `read(path)`, `write(path, text)`, `append(path, text)`, `exists(path)`, `files(folder)`, `run(command)` (returns its output), `env(name)` |

String positions and lengths count characters, not bytes. Paths may start
with `~`.

## Apps

In an app, `on*` attributes in `app.aui` name script functions. Each
function receives the widget that fired the event. A function called
`start()` runs once the window exists.

```xml
<window title="Counter" width="300" padding="16" spacing="10">
  <label id="count" text="0" size="huge" textalign="center"/>
  <button text="+" onclick="up"/>
</window>
```

```js
let count = 0;
fn up(button) { count += 1; ui.get("count").text = str(count); }
```

### Widgets

`ui.get(id)` returns a widget.

**Properties you can read and set:** `text`, `value`, `checked`,
`selected`, `enabled`, `visible`. Any other name reads or sets that AUI
attribute. You can also read `count`, `id` and `item` (the selected row's
text).

**Methods:**

| Method | Effect |
|---|---|
| `add(text...)` | Adds rows to a list, table or dropdown. |
| `clear()` | Removes every row. |
| `remove(i)` | Removes row `i`. |
| `get(i)` | Returns the text of row `i`. |
| `set(i, text)` | Replaces the text of row `i`. |
| `focus()` | Gives the widget the keyboard focus. |
| `insert(text)` | Inserts text at the cursor of a text field. |
| `on(event, fn)` | Sets a handler, for example `w.on("click", fn(w) { ... })`. |

### The ui functions

| Function | Returns |
|---|---|
| `ui.message(text [, title [, "A|B"]])` | The index of the button pressed. |
| `ui.confirm(text)` | `true` if the user chose Yes. |
| `ui.ask(question [, initial])` | The text typed, or `nil`. |
| `ui.open_file([title [, name [, "*.txt"]]])` | A path, or `nil`. |
| `ui.save_file([title [, name [, "*.txt"]]])` | A path, or `nil`. |
| `ui.timer(ms, fn)` | An id. The function runs repeatedly until it returns `false` or `ui.cancel(id)` is called. |
| `ui.title(text)` | Nothing. Sets the window title. |
| `ui.clipboard([text])` | The clipboard text. Sets it first when given text. |
| `ui.open(path)` | Whether the file or folder opened in its app. |
| `ui.quit()` | Nothing. Closes the app. |

`/osystem/resources/apprun/counter` is a complete example app.
`/osystem/resources/apprun/selftest.as` exercises the whole language. `make test`
runs it.
