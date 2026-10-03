// Dice: the sample .aip package.
fn roll(button) {
    let n = ui.get("count").selected + 1;
    let faces = "";
    let total = 0;
    for (i in n) {
        let d = random(6) + 1;
        total += d;
        faces = faces + str(d) + " ";
    }
    ui.get("face").text = faces;
    ui.get("total").text = "Total " + str(total);
}
fn start() { ui.get("count").selected = 1; }
