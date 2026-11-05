package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.throwsA;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Host test of JsonLite, the hand-written JSON reader and writer. Run with java -ea. */
public final class JsonLiteTest {
    private static Map<String, Object> map(Object... keysAndValues) {
        LinkedHashMap<String, Object> map = new LinkedHashMap<String, Object>();
        for (int i = 0; i < keysAndValues.length; i += 2) map.put((String) keysAndValues[i], keysAndValues[i + 1]);
        return map;
    }

    private static List<Object> list(Object... values) {
        return new ArrayList<Object>(Arrays.asList(values));
    }

    private static String repeat(String piece, int times) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < times; i++) out.append(piece);
        return out.toString();
    }

    private static void rejected(final String text) {
        throwsA(JsonLite.ParseException.class, "parse of <" + (text.length() > 40 ? text.substring(0, 40) + "..." : text) + ">",
                new SettingsCoreTestKit.Case() {
                    @Override public void run() throws Exception { JsonLite.parse(text); }
                });
    }

    private static void notWritable(final Object value, String what) {
        throwsA(IllegalArgumentException.class, "write of " + what, new SettingsCoreTestKit.Case() {
            @Override public void run() { JsonLite.write(value); }
        });
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("JsonLiteTest");

        test("parse: every kind of value, with the types the contract names", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                Object parsed = JsonLite.parse("{\"a\":1,\"b\":-2.5,\"c\":\"x\",\"d\":true,\"e\":false,\"f\":null,\"g\":[1,[2,{\"h\":[]}]],\"i\":{}}");
                same(map("a", 1L, "b", -2.5, "c", "x", "d", Boolean.TRUE, "e", Boolean.FALSE, "f", JsonLite.NULL,
                        "g", list(1L, list(2L, map("h", list()))), "i", map()), parsed, "value");
                check(parsed instanceof LinkedHashMap, "an object is a LinkedHashMap");
                same(Arrays.asList("a", "b", "c", "d", "e", "f", "g", "i"), new ArrayList<Object>(((Map<?, ?>) parsed).keySet()), "key order");
                check(((Map<?, ?>) parsed).get("g") instanceof ArrayList, "an array is an ArrayList");
                check(((Map<?, ?>) parsed).get("f") == JsonLite.NULL, "null is the NULL object, not Java null");
            }
        });

        test("parse: numbers (whole numbers are Long, the rest Double)", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                same(0L, JsonLite.parse("0"), "0");
                same(0L, JsonLite.parse("-0"), "-0");
                same(-17L, JsonLite.parse("-17"), "-17");
                same(Long.MAX_VALUE, JsonLite.parse("9223372036854775807"), "largest long");
                same(9.223372036854775808E18, JsonLite.parse("9223372036854775808"), "one above the largest long");
                same(1.5, JsonLite.parse("1.5"), "1.5");
                same(1000.0, JsonLite.parse("1e3"), "1e3");
                same(-0.0125, JsonLite.parse("-1.25E-2"), "-1.25E-2");
                same(100.0, JsonLite.parse("100.0"), "100.0 stays a Double");
            }
        });

        test("parse: strings and their escapes", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                same("a\"b\\c/d\b\f\n\r\t\u00e9\u20ac", JsonLite.parse("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\\u00e9\\u20AC\""), "escapes");
                same("caf\u00e9 \u20ac", JsonLite.parse("\"caf\u00e9 \u20ac\""), "characters above ASCII may stand unescaped");
                same("\ud83d\ude00", JsonLite.parse("\"\\ud83d\\ude00\""), "a surrogate pair");
                same("", JsonLite.parse("\"\""), "empty string");
            }
        });

        test("parse: blanks between tokens are allowed", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                same(map("a", list(1L, 2L)), JsonLite.parse(" \t\r\n{ \"a\" : [ 1 , 2 ] } \n"), "value");
            }
        });

        test("parse: malformed text is rejected", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String[] bad = {"", " ", "{", "}", "[", "[1,]", "[,1]", "[1 2]", "{\"a\":1,}", "{'a':1}", "{a:1}", "{\"a\" 1}",
                        "{\"a\":}", "{\"a\"}", "{1:2}", "01", "-01", "+1", ".5", "1.", "-", "1e", "1e+", "0x10", "tru", "nul", "True",
                        "NaN", "Infinity", "1E400", "\"abc", "\"a\nb\"", "\"a\tb\"", "\"\\x\"", "\"\\u12G4\"", "\"\\u12\"",
                        "\"\\u\uff11234\"", "'a'", "\ufeff{}"};
                for (String text : bad) rejected(text);
                throwsA(JsonLite.ParseException.class, "parse of Java null", new SettingsCoreTestKit.Case() {
                    @Override public void run() throws Exception { JsonLite.parse(null); }
                });
            }
        });

        test("parse: text after the value is rejected", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                for (String text : new String[] {"{\"a\":1}x", "{\"a\":1}{}", "1 2", "[1],", "\"a\"\"b\"", "true false", "{} garbage"}) rejected(text);
            }
        });

        test("parse: the same key twice in one object is rejected, in different objects it is fine", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                rejected("{\"a\":1,\"a\":2}");
                rejected("{\"a\":1,\"b\":{\"c\":1,\"c\":1}}");
                rejected("{\"a\":1,\"\\u0061\":2}"); // the same key, once written with an escape
                same(map("a", map("a", 1L)), JsonLite.parse("{\"a\":{\"a\":1}}"), "nested reuse");
                same(list(map("a", 1L), map("a", 2L)), JsonLite.parse("[{\"a\":1},{\"a\":2}]"), "sibling reuse");
            }
        });

        test("parse: 32 levels of nesting are accepted, 33 are rejected", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                JsonLite.parse(repeat("[", 32) + repeat("]", 32));
                rejected(repeat("[", 33) + repeat("]", 33));
                JsonLite.parse(repeat("{\"k\":", 32) + "1" + repeat("}", 32));
                rejected(repeat("{\"k\":", 33) + "1" + repeat("}", 33));
                rejected(repeat("[", 100000)); // far too deep and never closed: an error, not a stack overflow
            }
        });

        test("parse: exactly 1 MiB of text is accepted, one character more is rejected", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                String atLimit = "\"" + repeat("a", (1 << 20) - 2) + "\"";
                same(1 << 20, atLimit.length(), "test text length");
                same((1 << 20) - 2, ((String) JsonLite.parse(atLimit)).length(), "parsed length");
                rejected("\"" + repeat("a", (1 << 20) - 1) + "\"");
            }
        });

        test("parse: the position of the problem is reported", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                JsonLite.ParseException problem = throwsA(JsonLite.ParseException.class, "parse", new SettingsCoreTestKit.Case() {
                    @Override public void run() throws Exception { JsonLite.parse("[1,x]"); }
                });
                same(3, problem.position, "position");
            }
        });

        test("write: exact text (order kept, no spaces, escapes, plain ASCII)", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                Object value = map("b", 1L, "a", list(Boolean.TRUE, Boolean.FALSE, JsonLite.NULL, "x\"y\\z\n\u0001\u00e9/"), "c", map("d", -2.5), "e", 7);
                same("{\"b\":1,\"a\":[true,false,null,\"x\\\"y\\\\z\\n\\u0001\\u00e9/\"],\"c\":{\"d\":-2.5},\"e\":7}", JsonLite.write(value), "text");
                same("{}", JsonLite.write(map()), "empty object");
                same("[]", JsonLite.write(list()), "empty array");
            }
        });

        test("write then parse gives the value back, for every character", new SettingsCoreTestKit.Case() {
            @Override public void run() throws Exception {
                StringBuilder every = new StringBuilder();
                for (char c = 0; c < 0x250; c++) every.append(c);
                every.append('\u2028').append('\u2029').append('\ud83d').append('\ude00').append('\udc00').append('\uffff');
                Object value = map("text", every.toString(), "key \"with\" \\ and \n", list(1L, -1L, Long.MIN_VALUE, 0.1, -1.0E300, 1.0E-300),
                        "nested", map("list", list(list(), map(), JsonLite.NULL, Boolean.TRUE)), "", "");
                String text = JsonLite.write(value);
                for (int i = 0; i < text.length(); i++) check(text.charAt(i) >= 0x20 && text.charAt(i) <= 0x7e, "the written text is printable ASCII");
                same(value, JsonLite.parse(text), "round trip");
                same(text, JsonLite.write(JsonLite.parse(text)), "second round trip");
            }
        });

        test("write: what is not JSON is refused", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                notWritable(null, "Java null");
                notWritable(list("a", null), "Java null inside a list");
                notWritable(Double.NaN, "NaN");
                notWritable(Double.POSITIVE_INFINITY, "infinity");
                notWritable(new Object(), "an Object");
                notWritable('c', "a Character");
                LinkedHashMap<Object, Object> badKey = new LinkedHashMap<Object, Object>();
                badKey.put(1, "x");
                notWritable(badKey, "a map with a key that is not a String");
                Object deep = list();
                for (int i = 0; i < 32; i++) deep = list(deep);
                notWritable(deep, "33 levels of nesting");
                Object allowed = list();
                for (int i = 0; i < 31; i++) allowed = list(allowed);
                same(repeat("[", 32) + repeat("]", 32), JsonLite.write(allowed), "32 levels are written");
            }
        });

        SettingsCoreTestKit.end();
    }
}
