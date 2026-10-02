// A counter: the smallest AegisScript app.
let count = 0;

fn show() {
    ui.get("count").text = str(count);
    ui.get("note").text = "Last change " + date("%H:%M:%S");
}

fn up(button) { count += 1; show(); }
fn down(button) { count -= 1; show(); }
fn reset(button) {
    if (ui.confirm("Start again from zero?")) { count = 0; show(); }
}
fn start() { show(); }
