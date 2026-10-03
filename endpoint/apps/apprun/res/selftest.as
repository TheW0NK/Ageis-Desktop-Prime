// AegisScript self-test: prints "selftest: ok" if everything works.
let failures = 0;
fn check(name, got, want) {
    if (got != want) {
        print("FAIL", name, "got", got, "want", want);
        failures += 1;
    }
}

check("arithmetic", 1 + 2 * 3 - 4 / 2, 5);
check("precedence", (1 + 2) * 3, 9);
check("modulo", 17 % 5, 2);
check("strings", "ab" + "cd" + 1, "abcd1");
check("compare", "apple" < "banana", true);
check("logic", true && !false || false, true);
check("nil", nil == nil, true);

let list = [3, 1, 2];
push(list, 5);
check("list length", len(list), 4);
check("index", list[0] + list[-1], 8);
sort(list);
check("sort", join(list, ","), "1,2,3,5");
list[1] = 10;
check("set index", list[1], 10);

let m = {name: "Ada", "year": 1815};
m.lang = "Analytical";
check("map", m.name + " " + str(m["year"]), "Ada 1815");
check("map keys", len(keys(m)), 3);
check("has", has(m, "lang"), true);

let total = 0;
for (i in range(1, 11)) { total += i; }
check("for range", total, 55);
let words = 0;
for (w in split("the quick brown fox", " ")) { words += 1; }
check("for list", words, 4);
let n = 0;
while (true) { n += 1; if (n == 7) { break; } }
check("while break", n, 7);
let evens = 0;
for (i in 10) { if (i % 2) { continue; } evens += 1; }
check("continue", evens, 5);

fn fact(k) { if (k <= 1) { return 1; } return k * fact(k - 1); }
check("recursion", fact(10), 3628800);
fn counter() { let c = 0; return fn() { c += 1; return c; }; }
let next = counter();
next(); next();
check("closure", next(), 3);
let twice = fn(f, x) { return f(f(x)); };
check("higher order", twice(fn(x) { return x * 3; }, 2), 18);

check("upper", upper("aegis"), "AEGIS");
check("trim", trim("  hi  "), "hi");
check("replace", replace("a-b-c", "-", "+"), "a+b+c");
check("substr", substr("héllo", 1, 3), "éll");
check("unicode length", len("中文字"), 3);
check("find", find("hello", "ll"), 2);
check("num", num("3.5") + 1, 4.5);
check("format", format(3.14159, 2), "3.14");
check("int", int("42"), 42);
check("min max", min(4, 2, 9) + max([1, 7, 3]), 9);
check("pow", pow(2, 10), 1024);

write("/osystem/temp/selftest.txt", "line one\n");
append("/osystem/temp/selftest.txt", "line two\n");
check("files", len(split(trim(read("/osystem/temp/selftest.txt")), "\n")), 2);
check("run", trim(run("echo from the shell")), "from the shell");

if (failures == 0) { print("selftest: ok"); } else { print("selftest:", failures, "failures"); }
